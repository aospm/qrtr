/* Copyright (c) 2017, The Linux Foundation. All rights reserved.
 * Copyright (C) 2017-2018 Linaro Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *	* Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 *	* Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials provided
 *	  with the distribution.
 *	* Neither the name of The Linux Foundation nor the names of its
 *	  contributors may be used to endorse or promote products derived
 *	  from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libqrtr.h"
#include "logging.h"

#define QMI_ENCDEC_ENCODE_TLV(type, length, p_dst) do { \
	*p_dst++ = type; \
	*p_dst++ = ((uint8_t)((length) & 0xFF)); \
	*p_dst++ = ((uint8_t)(((length) >> 8) & 0xFF)); \
} while (0)

#define QMI_ENCDEC_DECODE_TLV(p_type, p_length, p_src) do { \
	*p_type = (uint8_t)*p_src++; \
	*p_length = (uint8_t)*p_src++; \
	*p_length |= ((uint8_t)*p_src) << 8; \
} while (0)

#define QMI_ENCDEC_ENCODE_N_BYTES(p_dst, p_src, size) \
do { \
	memcpy(p_dst, p_src, size); \
	p_dst = (uint8_t *)p_dst + size; \
	p_src = (uint8_t *)p_src + size; \
} while (0)

#define QMI_ENCDEC_DECODE_N_BYTES(p_dst, p_src, size) \
do { \
	memcpy(p_dst, p_src, size); \
	p_dst = (uint8_t *)p_dst + size; \
	p_src = (uint8_t *)p_src + size; \
} while (0)

#define UPDATE_ENCODE_VARIABLES(temp_si, buf_dst, \
				encoded_bytes, tlv_len, encode_tlv, rc) \
do { \
	buf_dst = (uint8_t *)buf_dst + rc; \
	encoded_bytes += rc; \
	tlv_len += rc; \
	temp_si = temp_si + 1; \
	encode_tlv = 1; \
} while (0)

#define UPDATE_DECODE_VARIABLES(buf_src, decoded_bytes, rc) \
do { \
	buf_src = (uint8_t *)buf_src + rc; \
	decoded_bytes += rc; \
} while (0)

#define TLV_LEN_SIZE sizeof(uint16_t)
#define TLV_TYPE_SIZE sizeof(uint8_t)
#define OPTIONAL_TLV_TYPE_START 0x10

static int qmi_encode(struct qmi_elem_info *ei_array, void *out_buf,
		      const void *in_c_struct, uint32_t out_buf_len,
		      int enc_level);

static int qmi_decode(struct qmi_elem_info *ei_array, void *out_c_struct,
		      const void *in_buf, uint32_t in_buf_len, int dec_level);

/**
 * skip_to_next_elem() - Skip to next element in the structure to be encoded
 * @ei_array: Struct info describing the element to be skipped.
 * @level: Depth level of encoding/decoding to identify nested structures.
 *
 * This function is used while encoding optional elements. If the flag
 * corresponding to an optional element is not set, then encoding the
 * optional element can be skipped. This function can be used to perform
 * that operation.
 *
 * Return: struct info of the next element that can be encoded.
 */
static struct qmi_elem_info *skip_to_next_elem(struct qmi_elem_info *ei_array,
					       int level)
{
	struct qmi_elem_info *temp_ei = ei_array;
	uint8_t tlv_type;

	if (level > 1) {
		temp_ei = temp_ei + 1;
	} else {
		do {
			tlv_type = temp_ei->tlv_type;
			temp_ei = temp_ei + 1;
		} while (tlv_type == temp_ei->tlv_type);
	}

	return temp_ei;
}

/**
 * qmi_calc_min_msg_len() - Calculate the minimum length of a QMI message
 * @ei_array: Struct info array describing the structure.
 * @level: Level to identify the depth of the nested structures.
 *
 * Return: Expected minimum length of the QMI message or 0 on error.
 */
static int qmi_calc_min_msg_len(struct qmi_elem_info *ei_array,
				int level)
{
	int min_msg_len = 0;
	struct qmi_elem_info *temp_ei = ei_array;

	if (!ei_array)
		return min_msg_len;

	while (temp_ei->data_type != QMI_EOTI) {
		/* Optional elements do not count in minimum length */
		if (temp_ei->data_type == QMI_OPT_FLAG) {
			temp_ei = skip_to_next_elem(temp_ei, level);
			continue;
		}

		if (temp_ei->data_type == QMI_DATA_LEN) {
			min_msg_len += (temp_ei->elem_size == sizeof(uint8_t) ?
					sizeof(uint8_t) : sizeof(uint16_t));
			temp_ei++;
			continue;
		} else if (temp_ei->data_type == QMI_STRUCT) {
			min_msg_len += qmi_calc_min_msg_len(temp_ei->ei_array,
							    (level + 1));
			temp_ei++;
		} else if (temp_ei->data_type == QMI_STRING) {
			if (level > 1)
				min_msg_len += temp_ei->elem_len <= 256 ?
					sizeof(uint8_t) : sizeof(uint16_t);
			min_msg_len += temp_ei->elem_len * temp_ei->elem_size;
			temp_ei++;
		} else {
			min_msg_len += (temp_ei->elem_len * temp_ei->elem_size);
			temp_ei++;
		}

		/*
		 * Type & Length info. not prepended for elements in the
		 * nested structure.
		 */
		if (level == 1)
			min_msg_len += (TLV_TYPE_SIZE + TLV_LEN_SIZE);
	}

	return min_msg_len;
}

/**
 * qmi_encode_basic_elem() - Encodes elements of basic/primary data type
 * @buf_dst: Buffer to store the encoded information.
 * @buf_src: Buffer containing the elements to be encoded.
 * @elem_len: Number of elements, in the buf_src, to be encoded.
 * @elem_size: Size of a single instance of the element to be encoded.
 *
 * This function encodes the "elem_len" number of data elements, each of
 * size "elem_size" bytes from the source buffer "buf_src" and stores the
 * encoded information in the destination buffer "buf_dst". The elements are
 * of primary data type which include uint8_t - u64 or similar. This
 * function returns the number of bytes of encoded information.
 *
 * Return: The number of bytes of encoded information.
 */
static int qmi_encode_basic_elem(void *buf_dst, const void *buf_src,
				 uint32_t elem_len, uint32_t elem_size)
{
	uint32_t i, rc = 0;

	for (i = 0; i < elem_len; i++) {
		QMI_ENCDEC_ENCODE_N_BYTES(buf_dst, buf_src, elem_size);
		rc += elem_size;
	}

	return rc;
}

/**
 * qmi_encode_struct_elem() - Encodes elements of struct data type
 * @ei_array: Struct info array descibing the struct element.
 * @buf_dst: Buffer to store the encoded information.
 * @buf_src: Buffer containing the elements to be encoded.
 * @elem_len: Number of elements, in the buf_src, to be encoded.
 * @out_buf_len: Available space in the encode buffer.
 * @enc_level: Depth of the nested structure from the main structure.
 *
 * This function encodes the "elem_len" number of struct elements, each of
 * size "ei_array->elem_size" bytes from the source buffer "buf_src" and
 * stores the encoded information in the destination buffer "buf_dst". The
 * elements are of struct data type which includes any C structure. This
 * function returns the number of bytes of encoded information.
 *
 * Return: The number of bytes of encoded information on success or negative
 * errno on error.
 */
static int qmi_encode_struct_elem(struct qmi_elem_info *ei_array,
				  void *buf_dst, const void *buf_src,
				  uint32_t elem_len, uint32_t out_buf_len,
				  int enc_level)
{
	int i, rc, encoded_bytes = 0;
	struct qmi_elem_info *temp_ei = ei_array;

	for (i = 0; i < elem_len; i++) {
		rc = qmi_encode(temp_ei->ei_array, buf_dst, buf_src,
				out_buf_len - encoded_bytes, enc_level);
		if (rc < 0) {
			LOGW("%s: STRUCT Encode failure\n", __func__);
			return rc;
		}
		buf_dst = (void*)((char*)buf_dst + rc);
		buf_src = (void*)((char*)buf_src + temp_ei->elem_size);
		encoded_bytes += rc;
	}

	return encoded_bytes;
}

/**
 * qmi_encode_string_elem() - Encodes elements of string data type
 * @ei_array: Struct info array descibing the string element.
 * @buf_dst: Buffer to store the encoded information.
 * @buf_src: Buffer containing the elements to be encoded.
 * @out_buf_len: Available space in the encode buffer.
 * @enc_level: Depth of the string element from the main structure.
 *
 * This function encodes a string element of maximum length "ei_array->elem_len"
 * bytes from the source buffer "buf_src" and stores the encoded information in
 * the destination buffer "buf_dst". This function returns the number of bytes
 * of encoded information.
 *
 * Return: The number of bytes of encoded information on success or negative
 * errno on error.
 */
static int qmi_encode_string_elem(struct qmi_elem_info *ei_array,
				  void *buf_dst, const void *buf_src,
				  uint32_t out_buf_len, int enc_level)
{
	int rc;
	int encoded_bytes = 0;
	struct qmi_elem_info *temp_ei = ei_array;
	uint32_t string_len = 0;
	uint32_t string_len_sz = 0;

	string_len = strlen(buf_src);
	string_len_sz = temp_ei->elem_len <= 256 ?
			sizeof(uint8_t) : sizeof(uint16_t);
	if (string_len > temp_ei->elem_len) {
		LOGW("%s: String to be encoded is longer - %d > %d\n",
		     __func__, string_len, temp_ei->elem_len);
		return -EINVAL;
	}

	if (enc_level == 1) {
		if (string_len + TLV_LEN_SIZE + TLV_TYPE_SIZE >
		    out_buf_len) {
			LOGW("%s: Output len %d > Out Buf len %d\n",
			     __func__, string_len, out_buf_len);
			return -EINVAL;
		}
	} else {
		if (string_len + string_len_sz > out_buf_len) {
			LOGW("%s: Output len %d > Out Buf len %d\n",
			     __func__, string_len, out_buf_len);
			return -EINVAL;
		}
		rc = qmi_encode_basic_elem(buf_dst, &string_len,
					   1, string_len_sz);
		encoded_bytes += rc;
	}

	rc = qmi_encode_basic_elem((void*)((char*)buf_dst + encoded_bytes), buf_src,
				   string_len, temp_ei->elem_size);
	encoded_bytes += rc;

	return encoded_bytes;
}

/**
 * qmi_encode() - Core Encode Function
 * @ei_array: Struct info array describing the structure to be encoded.
 * @out_buf: Buffer to hold the encoded QMI message.
 * @in_c_struct: Pointer to the C structure to be encoded.
 * @out_buf_len: Available space in the encode buffer.
 * @enc_level: Encode level to indicate the depth of the nested structure,
 *             within the main structure, being encoded.
 *
 * Return: The number of bytes of encoded information on success or negative
 * errno on error.
 */
static int qmi_encode(struct qmi_elem_info *ei_array, void *out_buf,
		      const void *in_c_struct, uint32_t out_buf_len,
		      int enc_level)
{
	struct qmi_elem_info *temp_ei = ei_array;
	uint8_t opt_flag_value = 0;
	uint32_t data_len_value = 0, data_len_sz;
	uint8_t *buf_dst = (uint8_t *)out_buf;
	uint8_t *tlv_pointer;
	uint32_t tlv_len;
	uint8_t tlv_type;
	uint32_t encoded_bytes = 0;
	const void *buf_src;
	int encode_tlv = 0;
	int rc;

	if (!ei_array)
		return 0;

	tlv_pointer = buf_dst;
	tlv_len = 0;
	if (enc_level == 1)
		buf_dst = buf_dst + (TLV_LEN_SIZE + TLV_TYPE_SIZE);

	while (temp_ei->data_type != QMI_EOTI) {
		buf_src = (void*)((char*)in_c_struct + temp_ei->offset);
		tlv_type = temp_ei->tlv_type;

		if (temp_ei->array_type == NO_ARRAY) {
			data_len_value = 1;
		} else if (temp_ei->array_type == STATIC_ARRAY) {
			data_len_value = temp_ei->elem_len;
		} else if (data_len_value <= 0 ||
			    temp_ei->elem_len < data_len_value) {
			LOGW("%s: Invalid data length\n", __func__);
			return -EINVAL;
		}

		switch (temp_ei->data_type) {
		case QMI_OPT_FLAG:
			rc = qmi_encode_basic_elem(&opt_flag_value, buf_src,
						   1, sizeof(uint8_t));
			if (opt_flag_value)
				temp_ei = temp_ei + 1;
			else
				temp_ei = skip_to_next_elem(temp_ei, enc_level);
			break;

		case QMI_DATA_LEN:
			memcpy(&data_len_value, buf_src, temp_ei->elem_size);
			data_len_sz = temp_ei->elem_size == sizeof(uint8_t) ?
					sizeof(uint8_t) : sizeof(uint16_t);
			/* Check to avoid out of range buffer access */
			if ((data_len_sz + encoded_bytes + TLV_LEN_SIZE +
			    TLV_TYPE_SIZE) > out_buf_len) {
				LOGW("%s: Too Small Buffer @DATA_LEN\n",
				       __func__);
				return -EINVAL;
			}
			rc = qmi_encode_basic_elem(buf_dst, &data_len_value,
						   1, data_len_sz);
			UPDATE_ENCODE_VARIABLES(temp_ei, buf_dst,
						encoded_bytes, tlv_len,
						encode_tlv, rc);
			if (!data_len_value)
				temp_ei = skip_to_next_elem(temp_ei, enc_level);
			else
				encode_tlv = 0;
			break;

		case QMI_UNSIGNED_1_BYTE:
		case QMI_UNSIGNED_2_BYTE:
		case QMI_UNSIGNED_4_BYTE:
		case QMI_UNSIGNED_8_BYTE:
		case QMI_SIGNED_1_BYTE:
		case QMI_SIGNED_2_BYTE:
		case QMI_SIGNED_4_BYTE:
		case QMI_SIGNED_8_BYTE:
		case QMI_SIGNED_1_BYTE_ENUM:
		case QMI_SIGNED_2_BYTE_ENUM:
		case QMI_SIGNED_4_BYTE_ENUM:
			/* Check to avoid out of range buffer access */
			if (((data_len_value * temp_ei->elem_size) +
			    encoded_bytes + TLV_LEN_SIZE + TLV_TYPE_SIZE) >
			    out_buf_len) {
				LOGW("%s: Too Small Buffer @data_type:%d\n",
				     __func__, temp_ei->data_type);
				return -EINVAL;
			}
			rc = qmi_encode_basic_elem(buf_dst, buf_src,
						   data_len_value,
						   temp_ei->elem_size);
			UPDATE_ENCODE_VARIABLES(temp_ei, buf_dst,
						encoded_bytes, tlv_len,
						encode_tlv, rc);
			break;

		case QMI_STRUCT:
			rc = qmi_encode_struct_elem(temp_ei, buf_dst, buf_src,
						    data_len_value,
						    out_buf_len - encoded_bytes,
						    enc_level + 1);
			if (rc < 0)
				return rc;
			UPDATE_ENCODE_VARIABLES(temp_ei, buf_dst,
						encoded_bytes, tlv_len,
						encode_tlv, rc);
			break;

		case QMI_STRING:
			rc = qmi_encode_string_elem(temp_ei, buf_dst, buf_src,
						    out_buf_len - encoded_bytes,
						    enc_level);
			if (rc < 0)
				return rc;
			UPDATE_ENCODE_VARIABLES(temp_ei, buf_dst,
						encoded_bytes, tlv_len,
						encode_tlv, rc);
			break;
		default:
			LOGW("%s: Unrecognized data type\n", __func__);
			return -EINVAL;
		}

		if (encode_tlv && enc_level == 1) {
			QMI_ENCDEC_ENCODE_TLV(tlv_type, tlv_len, tlv_pointer);
			encoded_bytes += (TLV_TYPE_SIZE + TLV_LEN_SIZE);
			tlv_pointer = buf_dst;
			tlv_len = 0;
			buf_dst = buf_dst + TLV_LEN_SIZE + TLV_TYPE_SIZE;
			encode_tlv = 0;
		}
	}

	return encoded_bytes;
}

/**
 * qmi_decode_basic_elem() - Decodes elements of basic/primary data type
 * @buf_dst: Buffer to store the decoded element.
 * @buf_src: Buffer containing the elements in QMI wire format.
 * @elem_len: Number of elements to be decoded.
 * @elem_size: Size of a single instance of the element to be decoded.
 *
 * This function decodes the "elem_len" number of elements in QMI wire format,
 * each of size "elem_size" bytes from the source buffer "buf_src" and stores
 * the decoded elements in the destination buffer "buf_dst". The elements are
 * of primary data type which include uint8_t - u64 or similar. This
 * function returns the number of bytes of decoded information.
 *
 * Return: The total size of the decoded data elements, in bytes.
 */
static int qmi_decode_basic_elem(void *buf_dst, const void *buf_src,
				 uint32_t elem_len, uint32_t elem_size)
{
	uint32_t i, rc = 0;

	for (i = 0; i < elem_len; i++) {
		QMI_ENCDEC_DECODE_N_BYTES(buf_dst, buf_src, elem_size);
		rc += elem_size;
	}

	return rc;
}

/**
 * qmi_decode_struct_elem() - Decodes elements of struct data type
 * @ei_array: Struct info array descibing the struct element.
 * @buf_dst: Buffer to store the decoded element.
 * @buf_src: Buffer containing the elements in QMI wire format.
 * @elem_len: Number of elements to be decoded.
 * @tlv_len: Total size of the encoded inforation corresponding to
 *           this struct element.
 * @dec_level: Depth of the nested structure from the main structure.
 *
 * This function decodes the "elem_len" number of elements in QMI wire format,
 * each of size "(tlv_len/elem_len)" bytes from the source buffer "buf_src"
 * and stores the decoded elements in the destination buffer "buf_dst". The
 * elements are of struct data type which includes any C structure. This
 * function returns the number of bytes of decoded information.
 *
 * Return: The total size of the decoded data elements on success, negative
 * errno on error.
 */
static int qmi_decode_struct_elem(struct qmi_elem_info *ei_array,
				  void *buf_dst, const void *buf_src,
				  uint32_t elem_len, uint32_t tlv_len,
				  int dec_level)
{
	int i, rc, decoded_bytes = 0;
	struct qmi_elem_info *temp_ei = ei_array;

	for (i = 0; i < elem_len && decoded_bytes < tlv_len; i++) {
		rc = qmi_decode(temp_ei->ei_array, buf_dst, buf_src,
				tlv_len - decoded_bytes, dec_level);
		if (rc < 0)
			return rc;
		buf_src = (void*)((char*)buf_src + rc);
		buf_dst = (void*)((char*)buf_dst + temp_ei->elem_size);
		decoded_bytes += rc;
	}

	if ((dec_level <= 2 && decoded_bytes != tlv_len) ||
	    (dec_level > 2 && (i < elem_len || decoded_bytes > tlv_len))) {
		LOGW("%s: Fault in decoding: dl(%d), db(%d), tl(%d), i(%d), el(%d)\n",
		     __func__, dec_level, decoded_bytes, tlv_len,
		     i, elem_len);
		return -EFAULT;
	}

	return decoded_bytes;
}

/**
 * qmi_decode_string_elem() - Decodes elements of string data type
 * @ei_array: Struct info array descibing the string element.
 * @buf_dst: Buffer to store the decoded element.
 * @buf_src: Buffer containing the elements in QMI wire format.
 * @tlv_len: Total size of the encoded inforation corresponding to
 *           this string element.
 * @dec_level: Depth of the string element from the main structure.
 *
 * This function decodes the string element of maximum length
 * "ei_array->elem_len" from the source buffer "buf_src" and puts it into
 * the destination buffer "buf_dst". This function returns number of bytes
 * decoded from the input buffer.
 *
 * Return: The total size of the decoded data elements on success, negative
 * errno on error.
 */
static int qmi_decode_string_elem(struct qmi_elem_info *ei_array,
				  void *buf_dst, const void *buf_src,
				  uint32_t tlv_len, int dec_level)
{
	int rc;
	int decoded_bytes = 0;
	uint32_t string_len = 0;
	uint32_t string_len_sz = 0;
	struct qmi_elem_info *temp_ei = ei_array;

	if (dec_level == 1) {
		string_len = tlv_len;
	} else {
		string_len_sz = temp_ei->elem_len <= 256 ?
				sizeof(uint8_t) : sizeof(uint16_t);
		rc = qmi_decode_basic_elem(&string_len, buf_src,
					   1, string_len_sz);
		decoded_bytes += rc;
	}

	if (string_len > temp_ei->elem_len) {
		LOGW("%s: String len %d > Max Len %d\n",
		     __func__, string_len, temp_ei->elem_len);
		return -EINVAL;
	} else if (string_len > tlv_len) {
		LOGW("%s: String len %d > Input Buffer Len %d\n",
		     __func__, string_len, tlv_len);
		return -EFAULT;
	}

	rc = qmi_decode_basic_elem(buf_dst, (void*)((char*)buf_src + decoded_bytes),
				   string_len, temp_ei->elem_size);
	*((char *)buf_dst + string_len) = '\0';
	decoded_bytes += rc;

	return decoded_bytes;
}

/**
 * find_ei() - Find element info corresponding to TLV Type
 * @ei_array: Struct info array of the message being decoded.
 * @type: TLV Type of the element being searched.
 *
 * Every element that got encoded in the QMI message will have a type
 * information associated with it. While decoding the QMI message,
 * this function is used to find the struct info regarding the element
 * that corresponds to the type being decoded.
 *
 * Return: Pointer to struct info, if found
 */
static struct qmi_elem_info *find_ei(struct qmi_elem_info *ei_array,
				     uint32_t type)
{
	struct qmi_elem_info *temp_ei = ei_array;

	while (temp_ei->data_type != QMI_EOTI) {
		if (temp_ei->tlv_type == (uint8_t)type)
			return temp_ei;
		temp_ei = temp_ei + 1;
	}

	return NULL;
}

/**
 * qmi_decode() - Core Decode Function
 * @ei_array: Struct info array describing the structure to be decoded.
 * @out_c_struct: Buffer to hold the decoded C struct
 * @in_buf: Buffer containing the QMI message to be decoded
 * @in_buf_len: Length of the QMI message to be decoded
 * @dec_level: Decode level to indicate the depth of the nested structure,
 *             within the main structure, being decoded
 *
 * Return: The number of bytes of decoded information on success, negative
 * errno on error.
 */
static int qmi_decode(struct qmi_elem_info *ei_array, void *out_c_struct,
		      const void *in_buf, uint32_t in_buf_len,
		      int dec_level)
{
	struct qmi_elem_info *temp_ei = ei_array;
	uint8_t opt_flag_value = 1;
	uint32_t data_len_value = 0, data_len_sz = 0;
	uint8_t *buf_dst = out_c_struct;
	const uint8_t *tlv_pointer;
	uint32_t tlv_len = 0;
	uint32_t tlv_type;
	uint32_t decoded_bytes = 0;
	const void *buf_src = in_buf;
	int rc;

	while (decoded_bytes < in_buf_len) {
		if (dec_level >= 2 && temp_ei->data_type == QMI_EOTI)
			return decoded_bytes;

		if (dec_level == 1) {
			tlv_pointer = buf_src;
			QMI_ENCDEC_DECODE_TLV(&tlv_type,
					      &tlv_len, tlv_pointer);
			buf_src = (void*)((char*)buf_src + (TLV_TYPE_SIZE + TLV_LEN_SIZE));
			decoded_bytes += (TLV_TYPE_SIZE + TLV_LEN_SIZE);
			temp_ei = find_ei(ei_array, tlv_type);
			if (!temp_ei && tlv_type < OPTIONAL_TLV_TYPE_START) {
				LOGW("%s: Inval element info\n", __func__);
				return -EINVAL;
			} else if (!temp_ei) {
				UPDATE_DECODE_VARIABLES(buf_src,
							decoded_bytes, tlv_len);
				continue;
			}
		} else {
			/*
			 * No length information for elements in nested
			 * structures. So use remaining decodable buffer space.
			 */
			tlv_len = in_buf_len - decoded_bytes;
		}

		buf_dst = (void*)((char*)out_c_struct + temp_ei->offset);
		if (temp_ei->data_type == QMI_OPT_FLAG) {
			memcpy(buf_dst, &opt_flag_value, sizeof(uint8_t));
			temp_ei = temp_ei + 1;
			buf_dst = (void*)((char*)out_c_struct + temp_ei->offset);
		}

		if (temp_ei->data_type == QMI_DATA_LEN) {
			data_len_sz = temp_ei->elem_size == sizeof(uint8_t) ?
					sizeof(uint8_t) : sizeof(uint16_t);
			rc = qmi_decode_basic_elem(&data_len_value, buf_src,
						   1, data_len_sz);
			memcpy(buf_dst, &data_len_value, sizeof(uint32_t));
			temp_ei = temp_ei + 1;
			buf_dst = (void*)((char*)out_c_struct + temp_ei->offset);
			tlv_len -= data_len_sz;
			UPDATE_DECODE_VARIABLES(buf_src, decoded_bytes, rc);
		}

		if (temp_ei->array_type == NO_ARRAY) {
			data_len_value = 1;
		} else if (temp_ei->array_type == STATIC_ARRAY) {
			data_len_value = temp_ei->elem_len;
		} else if (data_len_value > temp_ei->elem_len) {
			LOGW("%s: Data len %d > max spec %d\n",
			     __func__, data_len_value, temp_ei->elem_len);
			return -EINVAL;
		}

		switch (temp_ei->data_type) {
		case QMI_UNSIGNED_1_BYTE:
		case QMI_UNSIGNED_2_BYTE:
		case QMI_UNSIGNED_4_BYTE:
		case QMI_UNSIGNED_8_BYTE:
		case QMI_SIGNED_1_BYTE:
		case QMI_SIGNED_2_BYTE:
		case QMI_SIGNED_4_BYTE:
		case QMI_SIGNED_8_BYTE:
		case QMI_SIGNED_1_BYTE_ENUM:
		case QMI_SIGNED_2_BYTE_ENUM:
		case QMI_SIGNED_4_BYTE_ENUM:
			rc = qmi_decode_basic_elem(buf_dst, buf_src,
						   data_len_value,
						   temp_ei->elem_size);
			UPDATE_DECODE_VARIABLES(buf_src, decoded_bytes, rc);
			break;

		case QMI_STRUCT:
			rc = qmi_decode_struct_elem(temp_ei, buf_dst, buf_src,
						    data_len_value, tlv_len,
						    dec_level + 1);
			if (rc < 0)
				return rc;
			UPDATE_DECODE_VARIABLES(buf_src, decoded_bytes, rc);
			break;

		case QMI_STRING:
			rc = qmi_decode_string_elem(temp_ei, buf_dst, buf_src,
						    tlv_len, dec_level);
			if (rc < 0)
				return rc;
			UPDATE_DECODE_VARIABLES(buf_src, decoded_bytes, rc);
			break;

		default:
			LOGW("%s: Unrecognized data type\n", __func__);
			return -EINVAL;
		}
		temp_ei = temp_ei + 1;
	}

	return decoded_bytes;
}

/**
 * qmi_encode_message() - Encode C structure as QMI encoded message
 * @pkt:	QRTR packet to populate
 * @type:	Type of QMI message
 * @msg_id:	Message ID of the message
 * @txn_id:	Transaction ID
 * @ei:		QMI message descriptor
 * @c_struct:	Reference to structure to encode
 *
 * Return: Size of encoded data buffer, @pkt contains data.
 */
ssize_t qmi_encode_message(struct qrtr_packet *pkt, int type, int msg_id,
			   int txn_id, const void *c_struct,
			   struct qmi_elem_info *ei)
{
	struct qmi_header *hdr = pkt->data;
	ssize_t msglen = 0;
	int ret;

	/* Check the possibility of a zero length QMI message */
	if (!c_struct) {
		ret = qmi_calc_min_msg_len(ei, 1);
		if (ret) {
			LOGW("%s: Calc. len %d != 0, but NULL c_struct\n",
			     __func__, ret);
			return -EINVAL;
		}
	}

	if (pkt->data_len < sizeof(*hdr))
		return -EMSGSIZE;

	/* Encode message, if we have a message */
	if (c_struct) {
		msglen = qmi_encode(ei, (void*)((char*)pkt->data + sizeof(*hdr)), c_struct,
				    pkt->data_len - sizeof(*hdr), 1);
		if (msglen < 0)
			return msglen;
	}

	hdr->type = type;
	hdr->txn_id = txn_id;
	hdr->msg_id = msg_id;
	hdr->msg_len = msglen;

	pkt->type = QRTR_TYPE_DATA;
	pkt->data_len = sizeof(*hdr) + msglen;

	return pkt->data_len;
}

ssize_t qmi_encode_message2(void *buf, size_t bsz, int txn_id,
			    struct qmi_message_header *c_struct)
{
	struct qmi_header *msg_hdr = &c_struct->qmi_header;
	struct qmi_elem_info *ei = c_struct->ei;
	struct qrtr_packet pkt = { .data = buf, .data_len = bsz };
	ssize_t len = qmi_encode_message(&pkt,
		msg_hdr->type, msg_hdr->msg_id,
		txn_id, c_struct, ei);

	msg_hdr->txn_id = txn_id;
	if (len > 0)
		msg_hdr->msg_len = len - sizeof(struct qmi_header);
	return len;
}

const struct qmi_header *qmi_get_header(const struct qrtr_packet *pkt)
{
	const struct qmi_header *qmi = pkt->data;

	if (qmi->msg_len != pkt->data_len - sizeof(*qmi)) {
		LOGW("[QRTR] Invalid length of incoming qmi request\n");
		return NULL;
	}

	return qmi;
}

int qmi_decode_header(const struct qrtr_packet *pkt, unsigned int *msg_id)
{
	const struct qmi_header *qmi = qmi_get_header(pkt);

	*msg_id = qmi->msg_id;

	return 0;
}

int qmi_decode_header2(const struct qrtr_packet *pkt, unsigned int *msg_id, unsigned char *type,
		       unsigned short *txn_id)
{
	const struct qmi_header *qmi = qmi_get_header(pkt);
	if (!qmi)
		return -1;

	if (type)
		*type = qmi->type;
	if (msg_id)
		*msg_id = qmi->msg_id;
	if (txn_id)
		*txn_id = qmi->txn_id;

	return 0;
}

/**
 * qmi_decode_message() - Decode QMI encoded message to C structure
 * @buf:	Buffer with encoded message
 * @len:	Amount of data in @buf
 * @ei:		QMI message descriptor
 * @c_struct:	Reference to structure to decode into
 *
 * Return: The number of bytes of decoded information on success, negative
 * errno on error.
 */
int qmi_decode_message(void *c_struct, unsigned int *txn,
		       const struct qrtr_packet *pkt,
		       int type, int id, struct qmi_elem_info *ei)
{
	const struct qmi_header *hdr = pkt->data;

	if (!ei)
		return -EINVAL;

	if (!c_struct || !pkt->data || !pkt->data_len)
		return -EINVAL;

	if (hdr->type != type)
		return -EINVAL;

	if (hdr->msg_id != id)
		return -EINVAL;

	if (txn)
		*txn = hdr->txn_id;

	return qmi_decode(ei, c_struct, (void*)((char*)pkt->data + sizeof(*hdr)), pkt->data_len - sizeof(*hdr), 1);
}

ssize_t qmi_decode_message2(void *buf, size_t bsz,
			    struct qmi_message_header *c_struct)
{
	struct qmi_header *msg_hdr = &c_struct->qmi_header;
	struct qmi_elem_info *ei = c_struct->ei;
	unsigned int txn_id;
	struct qrtr_packet pkt = { .data = buf, .data_len = bsz };
	ssize_t len = qmi_decode_message(c_struct, &txn_id, &pkt,
		msg_hdr->type, msg_hdr->msg_id, ei);

	msg_hdr->txn_id = txn_id;
	return len;
}

/* Common header in all QMI responses */
struct qmi_elem_info qmi_response_type_v01_ei[] = {
	{
		.data_type	= QMI_SIGNED_2_BYTE_ENUM,
		.elem_len	= 1,
		.elem_size	= sizeof(uint16_t),
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
		.offset		= offsetof(struct qmi_response_type_v01, result),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_SIGNED_2_BYTE_ENUM,
		.elem_len	= 1,
		.elem_size	= sizeof(uint16_t),
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
		.offset		= offsetof(struct qmi_response_type_v01, error),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_EOTI,
		.elem_len	= 0,
		.elem_size	= 0,
		.array_type	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
		.offset		= 0,
		.ei_array	= NULL,
	},
};
