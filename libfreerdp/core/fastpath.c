/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Fast Path
 *
 * Copyright 2011 Vic Lee
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winpr/crt.h>
#include <winpr/stream.h>

#include <freerdp/api.h>
#include <freerdp/crypto/per.h>

#include "orders.h"
#include "update.h"
#include "surface.h"
#include "fastpath.h"
#include "rdp.h"

/**
 * Fast-Path packet format is defined in [MS-RDPBCGR] 2.2.9.1.2, which revises
 * server output packets from the first byte with the goal of improving
 * bandwidth.
 * 
 * Slow-Path packet always starts with TPKT header, which has the first
 * byte 0x03, while Fast-Path packet starts with 2 zero bits in the first
 * two less significant bits of the first byte.
 */

#define FASTPATH_MAX_PACKET_SIZE 0x3FFF

#ifdef WITH_DEBUG_RDP
static const char* const FASTPATH_UPDATETYPE_STRINGS[] =
{
	"Orders",									/* 0x0 */
	"Bitmap",									/* 0x1 */
	"Palette",								/* 0x2 */
	"Synchronize",						/* 0x3 */
	"Surface Commands",				/* 0x4 */
	"System Pointer Hidden",	/* 0x5 */
	"System Pointer Default",	/* 0x6 */
	"???",										/* 0x7 */
	"Pointer Position",				/* 0x8 */
	"Color Pointer",					/* 0x9 */
	"Cached Pointer",					/* 0xA */
	"New Pointer",						/* 0xB */
};
#endif

/*
 * The fastpath header may be two or three bytes long.
 * This function assumes that at least two bytes are available in the stream
 * and doesn't touch third byte.
 */
UINT16 fastpath_header_length(wStream* s)
{
	BYTE length1;

	Stream_Seek_UINT8(s);
	Stream_Read_UINT8(s, length1);
	Stream_Rewind(s, 2);

	return ((length1 & 0x80) != 0 ? 3 : 2);
}

/**
 * Read a Fast-Path packet header.\n
 * @param s stream
 * @param encryptionFlags
 * @return length
 */
UINT16 fastpath_read_header(rdpFastPath* fastpath, wStream* s)
{
	BYTE header;
	UINT16 length;

	Stream_Read_UINT8(s, header);

	if (fastpath)
	{
		fastpath->encryptionFlags = (header & 0xC0) >> 6;
		fastpath->numberEvents = (header & 0x3C) >> 2;
	}

	per_read_length(s, &length);

	return length;
}

static INLINE void fastpath_read_update_header(wStream* s, BYTE* updateCode, BYTE* fragmentation, BYTE* compression)
{
	BYTE updateHeader;

	Stream_Read_UINT8(s, updateHeader);
	*updateCode = updateHeader & 0x0F;
	*fragmentation = (updateHeader >> 4) & 0x03;
	*compression = (updateHeader >> 6) & 0x03;
}

INLINE void fastpath_write_update_header(wStream* s, FASTPATH_UPDATE_HEADER* fpUpdateHeader)
{
	fpUpdateHeader->updateHeader = 0;
	fpUpdateHeader->updateHeader |= fpUpdateHeader->updateCode & 0x0F;
	fpUpdateHeader->updateHeader |= (fpUpdateHeader->fragmentation & 0x03) << 4;
	fpUpdateHeader->updateHeader |= (fpUpdateHeader->compression & 0x03) << 6;

	Stream_Write_UINT8(s, fpUpdateHeader->updateHeader);

	if (fpUpdateHeader->compression)
		Stream_Write_UINT8(s, fpUpdateHeader->compressionFlags);

	Stream_Write_UINT16(s, fpUpdateHeader->size);
}

INLINE UINT32 fastpath_get_update_header_size(FASTPATH_UPDATE_HEADER* fpUpdateHeader)
{
	return (fpUpdateHeader->compression) ? 4 : 3;
}

static INLINE void fastpath_write_update_pdu_header(wStream* s, FASTPATH_UPDATE_PDU_HEADER* fpUpdatePduHeader)
{
	fpUpdatePduHeader->fpOutputHeader = 0;
	fpUpdatePduHeader->fpOutputHeader |= (fpUpdatePduHeader->action & 0x03);
	fpUpdatePduHeader->fpOutputHeader |= (fpUpdatePduHeader->secFlags & 0x03) << 6;

	Stream_Write_UINT8(s, fpUpdatePduHeader->fpOutputHeader); /* fpOutputHeader (1 byte) */

	Stream_Write_UINT8(s, 0x80 | (fpUpdatePduHeader->length >> 8)); /* length1 */
	Stream_Write_UINT8(s, fpUpdatePduHeader->length & 0xFF); /* length2 */
}

INLINE UINT32 fastpath_get_update_pdu_header_size(FASTPATH_UPDATE_PDU_HEADER* fpUpdatePduHeader)
{
	UINT32 size = 1;

	size += 2;

	if (fpUpdatePduHeader->secFlags)
	{
		size += 4;
		size += 8;
	}

	return size;
}

BOOL fastpath_read_header_rdp(rdpFastPath* fastpath, wStream* s, UINT16 *length)
{
	BYTE header;

	Stream_Read_UINT8(s, header);

	if (fastpath)
	{
		fastpath->encryptionFlags = (header & 0xC0) >> 6;
		fastpath->numberEvents = (header & 0x3C) >> 2;
	}

	if (!per_read_length(s, length))
		return FALSE;

	*length = *length - Stream_GetPosition(s);
	return TRUE;
}

static BOOL fastpath_recv_orders(rdpFastPath* fastpath, wStream* s)
{
	rdpUpdate* update = fastpath->rdp->update;
	UINT16 numberOrders;

	Stream_Read_UINT16(s, numberOrders); /* numberOrders (2 bytes) */

	while (numberOrders > 0)
	{
		if (!update_recv_order(update, s))
			return FALSE;

		numberOrders--;
	}

	return TRUE;
}

static BOOL fastpath_recv_update_common(rdpFastPath* fastpath, wStream* s)
{
	UINT16 updateType;
	rdpUpdate* update = fastpath->rdp->update;
	rdpContext* context = update->context;

	if (Stream_GetRemainingLength(s) < 2)
		return FALSE;

	Stream_Read_UINT16(s, updateType); /* updateType (2 bytes) */

	switch (updateType)
	{
		case UPDATE_TYPE_BITMAP:
			if (!update_read_bitmap_update(update, s, &update->bitmap_update))
				return FALSE;
			IFCALL(update->BitmapUpdate, context, &update->bitmap_update);
			break;

		case UPDATE_TYPE_PALETTE:
			if (!update_read_palette(update, s, &update->palette_update))
				return FALSE;
			IFCALL(update->Palette, context, &update->palette_update);
			break;
	}
	return TRUE;
}

static BOOL fastpath_recv_update_synchronize(rdpFastPath* fastpath, wStream* s)
{
	/* server 2008 can send invalid synchronize packet with missing padding,
	  so don't return FALSE even if the packet is invalid */
	Stream_SafeSeek(s, 2); /* size (2 bytes), MUST be set to zero */
	return TRUE;
}

static int fastpath_recv_update(rdpFastPath* fastpath, BYTE updateCode, UINT32 size, wStream* s)
{
	int status = 0;
	rdpUpdate* update = fastpath->rdp->update;
	rdpContext* context = fastpath->rdp->update->context;
	rdpPointerUpdate* pointer = update->pointer;

#ifdef WITH_DEBUG_RDP
	DEBUG_RDP("recv Fast-Path %s Update (0x%X), length:%d",
		updateCode < ARRAYSIZE(FASTPATH_UPDATETYPE_STRINGS) ? FASTPATH_UPDATETYPE_STRINGS[updateCode] : "???", updateCode, size);
#endif

	switch (updateCode)
	{
		case FASTPATH_UPDATETYPE_ORDERS:
			if (!fastpath_recv_orders(fastpath, s))
				return -1;
			break;

		case FASTPATH_UPDATETYPE_BITMAP:
		case FASTPATH_UPDATETYPE_PALETTE:
			if (!fastpath_recv_update_common(fastpath, s))
				return -1;
			break;

		case FASTPATH_UPDATETYPE_SYNCHRONIZE:
			if (!fastpath_recv_update_synchronize(fastpath, s))
				fprintf(stderr, "fastpath_recv_update_synchronize failure but we continue\n");
			else
				IFCALL(update->Synchronize, context);			
			break;

		case FASTPATH_UPDATETYPE_SURFCMDS:
			status = update_recv_surfcmds(update, size, s);
			break;

		case FASTPATH_UPDATETYPE_PTR_NULL:
			pointer->pointer_system.type = SYSPTR_NULL;
			IFCALL(pointer->PointerSystem, context, &pointer->pointer_system);
			break;

		case FASTPATH_UPDATETYPE_PTR_DEFAULT:
			update->pointer->pointer_system.type = SYSPTR_DEFAULT;
			IFCALL(pointer->PointerSystem, context, &pointer->pointer_system);

			break;

		case FASTPATH_UPDATETYPE_PTR_POSITION:
			if (!update_read_pointer_position(s, &pointer->pointer_position))
				return -1;
			IFCALL(pointer->PointerPosition, context, &pointer->pointer_position);
			break;

		case FASTPATH_UPDATETYPE_COLOR:
			if (!update_read_pointer_color(s, &pointer->pointer_color))
				return -1;
			IFCALL(pointer->PointerColor, context, &pointer->pointer_color);
			break;

		case FASTPATH_UPDATETYPE_CACHED:
			if (!update_read_pointer_cached(s, &pointer->pointer_cached))
				return -1;
			IFCALL(pointer->PointerCached, context, &pointer->pointer_cached);
			break;

		case FASTPATH_UPDATETYPE_POINTER:
			if (!update_read_pointer_new(s, &pointer->pointer_new))
				return -1;
			IFCALL(pointer->PointerNew, context, &pointer->pointer_new);
			break;

		default:
			DEBUG_WARN("unknown updateCode 0x%X", updateCode);
			break;
	}

	return status;
}

const char* fastpath_get_fragmentation_string(BYTE fragmentation)
{
	if (fragmentation == FASTPATH_FRAGMENT_SINGLE)
		return "FASTPATH_FRAGMENT_SINGLE";
	else if (fragmentation == FASTPATH_FRAGMENT_LAST)
		return "FASTPATH_FRAGMENT_LAST";
	else if (fragmentation == FASTPATH_FRAGMENT_FIRST)
		return "FASTPATH_FRAGMENT_FIRST";
	else if (fragmentation == FASTPATH_FRAGMENT_NEXT)
		return "FASTPATH_FRAGMENT_NEXT";

	return "FASTPATH_FRAGMENT_UNKNOWN";
}

static int fastpath_recv_update_data(rdpFastPath* fastpath, wStream* s)
{
	int status;
	UINT16 size;
	rdpRdp* rdp;
	int next_pos;
	wStream* cs;
	UINT32 totalSize;
	BYTE updateCode;
	BYTE fragmentation;
	BYTE compression;
	BYTE compressionFlags;
	rdpTransport* transport;

	status = 0;
	rdp = fastpath->rdp;
	transport = fastpath->rdp->transport;

	fastpath_read_update_header(s, &updateCode, &fragmentation, &compression);

	if (compression == FASTPATH_OUTPUT_COMPRESSION_USED)
		Stream_Read_UINT8(s, compressionFlags);
	else
		compressionFlags = 0;

	Stream_Read_UINT16(s, size);

	if (Stream_GetRemainingLength(s) < size)
		return -1;

	cs = s;
	next_pos = Stream_GetPosition(s) + size;

	if (compressionFlags & PACKET_COMPRESSED)
	{
		UINT32 DstSize = 0;
		BYTE* pDstData = NULL;

		if (bulk_decompress(rdp->bulk, Stream_Pointer(s), size, &pDstData, &DstSize, compressionFlags))
		{
			size = DstSize;
			cs = StreamPool_Take(transport->ReceivePool, DstSize);

			Stream_SetPosition(cs, 0);
			Stream_Write(cs, pDstData, DstSize);
			Stream_SealLength(cs);
			Stream_SetPosition(cs, 0);
		}
		else
		{
			fprintf(stderr, "bulk_decompress() failed\n");
			Stream_Seek(s, size);
		}
	}

	if (fragmentation == FASTPATH_FRAGMENT_SINGLE)
	{
		if (fastpath->fragmentation != -1)
		{
			fprintf(stderr, "Unexpected FASTPATH_FRAGMENT_SINGLE\n");
			return -1;
		}

		totalSize = size;
		status = fastpath_recv_update(fastpath, updateCode, totalSize, cs);

		if (status < 0)
			return -1;
	}
	else
	{
		if (fragmentation == FASTPATH_FRAGMENT_FIRST)
		{
			if (fastpath->fragmentation != -1)
			{
				fprintf(stderr, "Unexpected FASTPATH_FRAGMENT_FIRST\n");
				return -1;
			}

			fastpath->fragmentation = FASTPATH_FRAGMENT_FIRST;

			totalSize = size;

			if (totalSize > transport->settings->MultifragMaxRequestSize)
			{
				fprintf(stderr, "Total size (%d) exceeds MultifragMaxRequestSize (%d)\n",
						totalSize, transport->settings->MultifragMaxRequestSize);
				return -1;
			}

			fastpath->updateData = StreamPool_Take(transport->ReceivePool, size);
			Stream_SetPosition(fastpath->updateData, 0);

			Stream_Copy(fastpath->updateData, cs, size);
		}
		else if (fragmentation == FASTPATH_FRAGMENT_NEXT)
		{
			if ((fastpath->fragmentation != FASTPATH_FRAGMENT_FIRST) &&
					(fastpath->fragmentation != FASTPATH_FRAGMENT_NEXT))
			{
				fprintf(stderr, "Unexpected FASTPATH_FRAGMENT_NEXT\n");
				return -1;
			}

			fastpath->fragmentation = FASTPATH_FRAGMENT_NEXT;

			totalSize = Stream_GetPosition(fastpath->updateData) + size;

			if (totalSize > transport->settings->MultifragMaxRequestSize)
			{
				fprintf(stderr, "Total size (%d) exceeds MultifragMaxRequestSize (%d)\n",
						totalSize, transport->settings->MultifragMaxRequestSize);
				return -1;
			}

			Stream_EnsureCapacity(fastpath->updateData, totalSize);

			Stream_Copy(fastpath->updateData, cs, size);
		}
		else if (fragmentation == FASTPATH_FRAGMENT_LAST)
		{
			if ((fastpath->fragmentation != FASTPATH_FRAGMENT_FIRST) &&
					(fastpath->fragmentation != FASTPATH_FRAGMENT_NEXT))
			{
				fprintf(stderr, "Unexpected FASTPATH_FRAGMENT_LAST\n");
				return -1;
			}

			fastpath->fragmentation = -1;

			totalSize = Stream_GetPosition(fastpath->updateData) + size;

			if (totalSize > transport->settings->MultifragMaxRequestSize)
			{
				fprintf(stderr, "Total size (%d) exceeds MultifragMaxRequestSize (%d)\n",
						totalSize, transport->settings->MultifragMaxRequestSize);
				return -1;
			}

			Stream_EnsureCapacity(fastpath->updateData, totalSize);

			Stream_Copy(fastpath->updateData, cs, size);

			Stream_SealLength(fastpath->updateData);
			Stream_SetPosition(fastpath->updateData, 0);

			status = fastpath_recv_update(fastpath, updateCode, totalSize, fastpath->updateData);

			Stream_Release(fastpath->updateData);

			if (status < 0)
				return -1;
		}
	}

	Stream_SetPosition(s, next_pos);

	if (cs != s)
		Stream_Release(cs);

	return status;
}

int fastpath_recv_updates(rdpFastPath* fastpath, wStream* s)
{
	int status = 0;
	rdpUpdate* update = fastpath->rdp->update;

	IFCALL(update->BeginPaint, update->context);

	while (Stream_GetRemainingLength(s) >= 3)
	{
		if (fastpath_recv_update_data(fastpath, s) < 0)
			return -1;
	}

	IFCALL(update->EndPaint, update->context);

	return status;
}

static BOOL fastpath_read_input_event_header(wStream* s, BYTE* eventFlags, BYTE* eventCode)
{
	BYTE eventHeader;

	if (Stream_GetRemainingLength(s) < 1)
		return FALSE;

	Stream_Read_UINT8(s, eventHeader); /* eventHeader (1 byte) */

	*eventFlags = (eventHeader & 0x1F);
	*eventCode = (eventHeader >> 5);

	return TRUE;
}

static BOOL fastpath_recv_input_event_scancode(rdpFastPath* fastpath, wStream* s, BYTE eventFlags)
{
	UINT16 flags;
	UINT16 code;

	if (Stream_GetRemainingLength(s) < 1)
		return FALSE;

	Stream_Read_UINT8(s, code); /* keyCode (1 byte) */

	flags = 0;

	if ((eventFlags & FASTPATH_INPUT_KBDFLAGS_RELEASE))
		flags |= KBD_FLAGS_RELEASE;
	else
		flags |= KBD_FLAGS_DOWN;

	if ((eventFlags & FASTPATH_INPUT_KBDFLAGS_EXTENDED))
		flags |= KBD_FLAGS_EXTENDED;

	IFCALL(fastpath->rdp->input->KeyboardEvent, fastpath->rdp->input, flags, code);

	return TRUE;
}

static BOOL fastpath_recv_input_event_mouse(rdpFastPath* fastpath, wStream* s, BYTE eventFlags)
{
	UINT16 pointerFlags;
	UINT16 xPos;
	UINT16 yPos;

	if (Stream_GetRemainingLength(s) < 6)
		return FALSE;

	Stream_Read_UINT16(s, pointerFlags); /* pointerFlags (2 bytes) */
	Stream_Read_UINT16(s, xPos); /* xPos (2 bytes) */
	Stream_Read_UINT16(s, yPos); /* yPos (2 bytes) */

	IFCALL(fastpath->rdp->input->MouseEvent, fastpath->rdp->input, pointerFlags, xPos, yPos);

	return TRUE;
}

static BOOL fastpath_recv_input_event_mousex(rdpFastPath* fastpath, wStream* s, BYTE eventFlags)
{
	UINT16 pointerFlags;
	UINT16 xPos;
	UINT16 yPos;

	if (Stream_GetRemainingLength(s) < 6)
		return FALSE;

	Stream_Read_UINT16(s, pointerFlags); /* pointerFlags (2 bytes) */
	Stream_Read_UINT16(s, xPos); /* xPos (2 bytes) */
	Stream_Read_UINT16(s, yPos); /* yPos (2 bytes) */

	IFCALL(fastpath->rdp->input->ExtendedMouseEvent, fastpath->rdp->input, pointerFlags, xPos, yPos);

	return TRUE;
}

static BOOL fastpath_recv_input_event_sync(rdpFastPath* fastpath, wStream* s, BYTE eventFlags)
{
	IFCALL(fastpath->rdp->input->SynchronizeEvent, fastpath->rdp->input, eventFlags);

	return TRUE;
}

static BOOL fastpath_recv_input_event_unicode(rdpFastPath* fastpath, wStream* s, BYTE eventFlags)
{
	UINT16 unicodeCode;
	UINT16 flags;

	if (Stream_GetRemainingLength(s) < 2)
		return FALSE;

	Stream_Read_UINT16(s, unicodeCode); /* unicodeCode (2 bytes) */

	flags = 0;

	if ((eventFlags & FASTPATH_INPUT_KBDFLAGS_RELEASE))
		flags |= KBD_FLAGS_RELEASE;
	else
		flags |= KBD_FLAGS_DOWN;

	IFCALL(fastpath->rdp->input->UnicodeKeyboardEvent, fastpath->rdp->input, flags, unicodeCode);

	return TRUE;
}

static BOOL fastpath_recv_input_event(rdpFastPath* fastpath, wStream* s)
{
	BYTE eventFlags;
	BYTE eventCode;

	if (!fastpath_read_input_event_header(s, &eventFlags, &eventCode))
		return FALSE;

	switch (eventCode)
	{
		case FASTPATH_INPUT_EVENT_SCANCODE:
			if (!fastpath_recv_input_event_scancode(fastpath, s, eventFlags))
				return FALSE;
			break;

		case FASTPATH_INPUT_EVENT_MOUSE:
			if (!fastpath_recv_input_event_mouse(fastpath, s, eventFlags))
				return FALSE;
			break;

		case FASTPATH_INPUT_EVENT_MOUSEX:
			if (!fastpath_recv_input_event_mousex(fastpath, s, eventFlags))
				return FALSE;
			break;

		case FASTPATH_INPUT_EVENT_SYNC:
			if (!fastpath_recv_input_event_sync(fastpath, s, eventFlags))
				return FALSE;
			break;

		case FASTPATH_INPUT_EVENT_UNICODE:
			if (!fastpath_recv_input_event_unicode(fastpath, s, eventFlags))
				return FALSE;
			break;

		default:
			fprintf(stderr, "Unknown eventCode %d\n", eventCode);
			break;
	}

	return TRUE;
}

int fastpath_recv_inputs(rdpFastPath* fastpath, wStream* s)
{
	BYTE i;

	if (fastpath->numberEvents == 0)
	{
		/**
		 * If numberEvents is not provided in fpInputHeader, it will be provided
		 * as one additional byte here.
		 */

		if (Stream_GetRemainingLength(s) < 1)
			return -1;

		Stream_Read_UINT8(s, fastpath->numberEvents); /* eventHeader (1 byte) */
	}

	for (i = 0; i < fastpath->numberEvents; i++)
	{
		if (!fastpath_recv_input_event(fastpath, s))
			return -1;
	}

	return 0;
}

static UINT32 fastpath_get_sec_bytes(rdpRdp* rdp)
{
	UINT32 sec_bytes;

	sec_bytes = 0;

	if (rdp->do_crypt)
	{
		sec_bytes = 8;

		if (rdp->settings->EncryptionMethods == ENCRYPTION_METHOD_FIPS)
			sec_bytes += 4;
	}

	return sec_bytes;
}

wStream* fastpath_input_pdu_init_header(rdpFastPath* fastpath)
{
	rdpRdp *rdp;
	wStream* s;

	rdp = fastpath->rdp;

	s = transport_send_stream_init(rdp->transport, 256);

	Stream_Seek(s, 3); /* fpInputHeader, length1 and length2 */

	if (rdp->do_crypt)
	{
		rdp->sec_flags |= SEC_ENCRYPT;

		if (rdp->do_secure_checksum)
			rdp->sec_flags |= SEC_SECURE_CHECKSUM;
	}

	Stream_Seek(s, fastpath_get_sec_bytes(rdp));

	return s;
}

wStream* fastpath_input_pdu_init(rdpFastPath* fastpath, BYTE eventFlags, BYTE eventCode)
{
	rdpRdp *rdp;
	wStream* s;

	rdp = fastpath->rdp;

	s = fastpath_input_pdu_init_header(fastpath);
	Stream_Write_UINT8(s, eventFlags | (eventCode << 5)); /* eventHeader (1 byte) */

	return s;
}

BOOL fastpath_send_multiple_input_pdu(rdpFastPath* fastpath, wStream* s, int iNumEvents)
{
	rdpRdp* rdp;
	UINT16 length;
	BYTE eventHeader;
	int sec_bytes;

	/*
	 *  A maximum of 15 events are allowed per request
	 *  if the optional numEvents field isn't used
	 *  see MS-RDPBCGR 2.2.8.1.2 for details
	 */
	if (iNumEvents > 15)
		return FALSE;

	rdp = fastpath->rdp;

	length = Stream_GetPosition(s);

	if (length >= (2 << 14))
	{
		fprintf(stderr, "Maximum FastPath PDU length is 32767\n");
		return FALSE;
	}

	eventHeader = FASTPATH_INPUT_ACTION_FASTPATH;
	eventHeader |= (iNumEvents << 2); /* numberEvents */

	if (rdp->sec_flags & SEC_ENCRYPT)
		eventHeader |= (FASTPATH_INPUT_ENCRYPTED << 6);
	if (rdp->sec_flags & SEC_SECURE_CHECKSUM)
		eventHeader |= (FASTPATH_INPUT_SECURE_CHECKSUM << 6);

	Stream_SetPosition(s, 0);
	Stream_Write_UINT8(s, eventHeader);
	sec_bytes = fastpath_get_sec_bytes(fastpath->rdp);

	/*
	 * We always encode length in two bytes, even though we could use
	 * only one byte if length <= 0x7F. It is just easier that way,
	 * because we can leave room for fixed-length header, store all
	 * the data first and then store the header.
	 */
	Stream_Write_UINT16_BE(s, 0x8000 | length);

	if (sec_bytes > 0)
	{
		BYTE* fpInputEvents;
		UINT16 fpInputEvents_length;

		fpInputEvents = Stream_Pointer(s) + sec_bytes;
		fpInputEvents_length = length - 3 - sec_bytes;

		if (rdp->sec_flags & SEC_SECURE_CHECKSUM)
			security_salted_mac_signature(rdp, fpInputEvents, fpInputEvents_length, TRUE, Stream_Pointer(s));
		else
			security_mac_signature(rdp, fpInputEvents, fpInputEvents_length, Stream_Pointer(s));

		security_encrypt(fpInputEvents, fpInputEvents_length, rdp);
	}

	rdp->sec_flags = 0;

	Stream_SetPosition(s, length);
	Stream_SealLength(s);

	if (transport_write(fastpath->rdp->transport, s) < 0)
		return FALSE;

	return TRUE;
}

BOOL fastpath_send_input_pdu(rdpFastPath* fastpath, wStream* s)
{
	return fastpath_send_multiple_input_pdu(fastpath, s, 1);
}

wStream* fastpath_update_pdu_init(rdpFastPath* fastpath)
{
	wStream* s;
	s = transport_send_stream_init(fastpath->rdp->transport, FASTPATH_MAX_PACKET_SIZE);
	return s;
}

wStream* fastpath_update_pdu_init_new(rdpFastPath* fastpath)
{
	wStream* s;
	s = Stream_New(NULL, FASTPATH_MAX_PACKET_SIZE);
	return s;
}

BOOL fastpath_send_update_pdu(rdpFastPath* fastpath, BYTE updateCode, wStream* s)
{
	int fragment;
	UINT16 maxLength;
	UINT32 totalLength;
	BOOL status = TRUE;
	wStream* fs = NULL;
	rdpSettings* settings;
	rdpRdp* rdp = fastpath->rdp;
	UINT32 fpHeaderSize = 6;
	UINT32 fpUpdatePduHeaderSize;
	UINT32 fpUpdateHeaderSize;
	UINT32 CompressionMaxSize;
	FASTPATH_UPDATE_PDU_HEADER fpUpdatePduHeader = { 0 };
	FASTPATH_UPDATE_HEADER fpUpdateHeader = { 0 };

	fs = fastpath->fs;
	settings = rdp->settings;

	maxLength = FASTPATH_MAX_PACKET_SIZE - 20;

	if (settings->CompressionEnabled)
	{
		CompressionMaxSize = bulk_compression_max_size(rdp->bulk);
		maxLength = (maxLength < CompressionMaxSize) ? maxLength : CompressionMaxSize;
	}

	totalLength = Stream_GetPosition(s);
	Stream_SetPosition(s, 0);

	for (fragment = 0; (totalLength > 0) || (fragment == 0); fragment++)
	{
		BYTE* pSrcData;
		UINT32 SrcSize;
		UINT32 DstSize = 0;
		BYTE* pDstData = NULL;
		UINT32 compressionFlags = 0;

		fpUpdatePduHeader.action = 0;
		fpUpdatePduHeader.secFlags = 0;

		fpUpdateHeader.compression = 0;
		fpUpdateHeader.compressionFlags = 0;
		fpUpdateHeader.updateCode = updateCode;
		fpUpdateHeader.size = (totalLength > maxLength) ? maxLength : totalLength;

		pSrcData = pDstData = Stream_Pointer(s);
		SrcSize = DstSize = fpUpdateHeader.size;

		if (settings->CompressionEnabled)
		{
			if (bulk_compress(rdp->bulk, pSrcData, SrcSize, &pDstData, &DstSize, &compressionFlags) >= 0)
			{
				if (compressionFlags & PACKET_COMPRESSED)
				{
					fpUpdateHeader.compressionFlags = compressionFlags;
					fpUpdateHeader.compression = FASTPATH_OUTPUT_COMPRESSION_USED;
				}
			}
		}

		if (!fpUpdateHeader.compression)
		{
			pDstData = Stream_Pointer(s);
			DstSize = fpUpdateHeader.size;
		}

		fpUpdateHeader.size = DstSize;
		totalLength -= SrcSize;

		if (totalLength == 0)
			fpUpdateHeader.fragmentation = (fragment == 0) ? FASTPATH_FRAGMENT_SINGLE : FASTPATH_FRAGMENT_LAST;
		else
			fpUpdateHeader.fragmentation = (fragment == 0) ? FASTPATH_FRAGMENT_FIRST : FASTPATH_FRAGMENT_NEXT;

		fpUpdateHeaderSize = fastpath_get_update_header_size(&fpUpdateHeader);
		fpUpdatePduHeaderSize = fastpath_get_update_pdu_header_size(&fpUpdatePduHeader);
		fpHeaderSize = fpUpdateHeaderSize + fpUpdatePduHeaderSize;
		fpUpdatePduHeader.length = fpUpdateHeader.size + fpHeaderSize;

		Stream_SetPosition(fs, 0);

		fastpath_write_update_pdu_header(fs, &fpUpdatePduHeader);
		fastpath_write_update_header(fs, &fpUpdateHeader);

		Stream_Write(fs, pDstData, DstSize);
		Stream_SealLength(fs);

		if (transport_write(rdp->transport, fs) < 0)
		{
			status = FALSE;
			break;
		}

		Stream_Seek(s, SrcSize);
	}

	return status;
}

rdpFastPath* fastpath_new(rdpRdp* rdp)
{
	rdpFastPath* fastpath;

	fastpath = (rdpFastPath*) malloc(sizeof(rdpFastPath));

	if (fastpath)
	{
		ZeroMemory(fastpath, sizeof(rdpFastPath));

		fastpath->rdp = rdp;
		fastpath->fragmentation = -1;
		fastpath->fs = Stream_New(NULL, FASTPATH_MAX_PACKET_SIZE);
	}

	return fastpath;
}

void fastpath_free(rdpFastPath* fastpath)
{
	if (fastpath)
	{
		Stream_Free(fastpath->fs, TRUE);
		free(fastpath);
	}
}
