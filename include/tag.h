/*
 *
 *  neard - Near Field Communication manager
 *
 *  Copyright (C) 2011  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef __NEAR_TAG_H
#define __NEAR_TAG_H

#include <stdint.h>

#include <glib.h>

#define NFC_HEADER_SIZE 1

#define	NEAR_TAG_NFC_TYPE1   0x1
#define	NEAR_TAG_NFC_TYPE2   0x2
#define	NEAR_TAG_NFC_TYPE3   0x4
#define	NEAR_TAG_NFC_TYPE4   0x8
#define	NEAR_TAG_NFC_DEP     0x10
#define	NEAR_TAG_NFC_UNKNOWN 0xff

enum near_target_sub_type {
	NEAR_TAG_NFC_T2_MIFARE_ULTRALIGHT = 0,	// SAK 0x00
	NEAR_TAG_NFC_T2_MIFARE_1K,		// SAK:0x08
	NEAR_TAG_NFC_T2_MIFARE_MINI,		// SAK 0x09
	NEAR_TAG_NFC_T2_MIFARE_STD_4K,		// SAK:0x18
	NEAR_TAG_NFC_T2_MIFARE_DESFIRE,		// SAK:0x20
	NEAR_TAG_NFC_T2_JCOP30,			// SAK:0x28
	NEAR_TAG_NFC_T2_MIFARE_4K_EMUL,		// SAK:0x38
	NEAR_TAG_NFC_T2_MIFARE_1K_INFINEON,	// SAK:0x88
	NEAR_TAG_NFC_T2_MPCOS,			// SAK:0x98
	NEAR_TAG_NFC_SUBTYPE_UNKNOWN = 0xFF
};

enum near_tag_memory_layout {
	NEAR_TAG_MEMORY_STATIC = 0,
	NEAR_TAG_MEMORY_DYNAMIC,
	NEAR_TAG_MEMORY_OTHER,
	NEAR_TAG_MEMORY_UNKNOWN = 0xFF
};

typedef void (*near_tag_io_cb) (uint32_t adapter_idx, uint32_t target_idx,
								int status);

struct near_ndef_message;

#define NEAR_TAG_PRIORITY_LOW      -100
#define NEAR_TAG_PRIORITY_DEFAULT     0
#define NEAR_TAG_PRIORITY_HIGH      100

struct near_tag_driver {
	uint16_t type;
	int priority;

	int (*read_tag)(uint32_t adapter_idx, uint32_t target_idx,
						near_tag_io_cb cb);
	int (*add_ndef)(uint32_t adapter_idx, uint32_t target_idx,
					struct near_ndef_message *ndef,
					near_tag_io_cb cb);
};

struct near_tag;

int near_tag_set_uid(struct near_tag *tag, uint8_t *uid, size_t uid_length);
int near_tag_set_ro(struct near_tag *tag, near_bool_t readonly);
near_bool_t near_tag_get_ro(struct near_tag *tag);
uint8_t *near_tag_get_data(struct near_tag *tag, size_t *data_length);
uint32_t near_tag_get_adapter_idx(struct near_tag *tag);
uint32_t near_tag_get_target_idx(struct near_tag *tag);
int near_tag_add_ndef(struct near_tag *tag, uint8_t *ndef_data, size_t ndef_length);
int near_tag_driver_register(struct near_tag_driver *driver);
void near_tag_driver_unregister(struct near_tag_driver *driver);
void near_tag_set_memory_layout(struct near_tag *tag,
					enum near_tag_memory_layout);
enum near_tag_memory_layout near_tag_get_memory_layout(struct near_tag *tag);
void near_tag_set_max_ndef_size(struct near_tag *tag, uint16_t size);
uint16_t near_tag_get_max_ndef_size(struct near_tag *tag);
void near_tag_set_c_apdu_max_size(struct near_tag *tag, uint16_t size);
uint16_t near_tag_get_c_apdu_max_size(struct near_tag *tag);
void near_tag_set_idm(struct near_tag *tag, uint8_t *idm, uint8_t len);
uint8_t *near_tag_get_idm(struct near_tag *tag, uint8_t *len);
void near_tag_set_attr_block(struct near_tag *tag, uint8_t *attr, uint8_t len);
uint8_t *near_tag_get_attr_block(struct near_tag *tag, uint8_t *len);

#endif
