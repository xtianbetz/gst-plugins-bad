/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) 2004,2006 Thomas Vander Stichele <thomas at apestaart dot org>
 *
 * dataprotocol.c: Functions implementing the GStreamer Data Protocol
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:gstdataprotocol
 * @short_description: Serialization of caps, buffers and events.
 * @see_also: #GstCaps, #GstEvent, #GstBuffer
 *
 * This helper library provides serialization of GstBuffer, GstCaps and
 * GstEvent structures.
 *
 * This serialization is useful when GStreamer needs to interface with
 * the outside world to transport data between distinct GStreamer pipelines.
 * The connections with the outside world generally don't have mechanisms
 * to transport properties of these structures.
 *
 * For example, transporting buffers across named pipes or network connections
 * doesn't maintain the buffer size and separation.
 *
 * This data protocol assumes a reliable connection-oriented transport, such as
 * TCP, a pipe, or a file.  The protocol does not serialize the caps for
 * each buffer; instead, it transport the caps only when they change in the
 * stream.  This implies that there will always be a caps packet before any
 * buffer packets.
 *
 * The versioning of the protocol is independent of GStreamer's version.
 * The major number gets incremented, and the minor reset, for incompatible
 * changes.  The minor number gets incremented for compatible changes that
 * allow clients who do not completely understand the newer protocol version
 * to still decode what they do understand.
 *
 * Version 0.2 serializes only a small subset of all events, with a custom
 * payload for each type.  Also, all GDP streams start with the initial caps
 * packet.
 *
 * Version 1.0 serializes all events by taking the string representation of
 * the event as the payload.  In addition, GDP streams can now start with
 * events as well, as required by the new data stream model in GStreamer 0.10.
 *
 * Converting buffers, caps and events to GDP buffers is done using a
 * #GstDPPacketizer object and invoking its packetizer functions.
 * For backwards-compatibility reasons, the old 0.2 methods are still
 * available but deprecated.
 *
 * For reference, this image shows the byte layout of the GDP header:
 *
 * <inlinegraphic format="PNG" fileref="gdp-header.png"></inlinegraphic>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/dataprotocol/dataprotocol.h>
#include <glib/gprintf.h>       /* g_sprintf */
#include <string.h>             /* strlen */
#include "dp-private.h"

/* debug category */
GST_DEBUG_CATEGORY (data_protocol_debug);
#define GST_CAT_DEFAULT data_protocol_debug

/* helper macros */

/* write first 6 bytes of header, as well as ABI padding */
#define GST_DP_INIT_HEADER(h, version, flags, type)		\
G_STMT_START {							\
  gint maj = 0, min = 0;					\
  switch (version) {						\
    case GST_DP_VERSION_0_2: maj = 0; min = 2; break;		\
    case GST_DP_VERSION_1_0: maj = 1; min = 0; break;		\
  }								\
  h[0] = (guint8) maj;						\
  h[1] = (guint8) min;						\
  h[2] = (guint8) flags;					\
  h[3] = 0; /* padding byte */					\
  GST_WRITE_UINT16_BE (h + 4, type);				\
								\
  GST_WRITE_UINT64_BE (h + 42, (guint64) 0); /* ABI padding */	\
  GST_WRITE_UINT64_BE (h + 50, (guint64) 0); /* ABI padding */	\
} G_STMT_END

#define GST_DP_SET_CRC(h, flags, payload, length);		\
G_STMT_START {							\
  guint16 crc = 0;						\
  if (flags & GST_DP_HEADER_FLAG_CRC_HEADER)			\
    /* we don't crc the last four bytes since they are crc's */ \
    crc = gst_dp_crc (h, 58);					\
  GST_WRITE_UINT16_BE (h + 58, crc);				\
								\
  crc = 0;							\
  if (length && (flags & GST_DP_HEADER_FLAG_CRC_PAYLOAD))	\
    crc = gst_dp_crc (payload, length);				\
  GST_WRITE_UINT16_BE (h + 60, crc);				\
} G_STMT_END

/* calculate a CCITT 16 bit CRC check value for a given byte array */
/*
 * this code snippet is adapted from a web page I found
 * it is identical except for cleanups, and a final XOR with 0xffff
 * as outlined in the uecp spec
 *
 * XMODEM    x^16 + x^12 + x^5 + 1
 */

#define POLY       0x1021
#define CRC_INIT   0xFFFF

/*** HELPER FUNCTIONS ***/

static gboolean
gst_dp_header_from_buffer_any (const GstBuffer * buffer, GstDPHeaderFlag flags,
    guint * length, guint8 ** header, GstDPVersion version)
{
  guint8 *h;
  guint16 flags_mask;

  g_return_val_if_fail (GST_IS_BUFFER (buffer), FALSE);
  g_return_val_if_fail (header, FALSE);

  *length = GST_DP_HEADER_LENGTH;
  h = g_malloc0 (GST_DP_HEADER_LENGTH);

  /* version, flags, type */
  GST_DP_INIT_HEADER (h, version, flags, GST_DP_PAYLOAD_BUFFER);

  /* buffer properties */
  GST_WRITE_UINT32_BE (h + 6, GST_BUFFER_SIZE (buffer));
  GST_WRITE_UINT64_BE (h + 10, GST_BUFFER_TIMESTAMP (buffer));
  GST_WRITE_UINT64_BE (h + 18, GST_BUFFER_DURATION (buffer));
  GST_WRITE_UINT64_BE (h + 26, GST_BUFFER_OFFSET (buffer));
  GST_WRITE_UINT64_BE (h + 34, GST_BUFFER_OFFSET_END (buffer));

  /* data flags; eats two bytes from the ABI area */
  /* we copy everything but the read-only flags */
  flags_mask = GST_BUFFER_FLAG_PREROLL | GST_BUFFER_FLAG_DISCONT |
      GST_BUFFER_FLAG_IN_CAPS | GST_BUFFER_FLAG_GAP |
      GST_BUFFER_FLAG_DELTA_UNIT;

  GST_WRITE_UINT16_BE (h + 42, GST_BUFFER_FLAGS (buffer) & flags_mask);

  GST_DP_SET_CRC (h, flags, GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer));

  GST_LOG ("created header from buffer:");
  gst_dp_dump_byte_array (h, GST_DP_HEADER_LENGTH);
  *header = h;
  return TRUE;
}

static gboolean
gst_dp_packet_from_caps_any (const GstCaps * caps, GstDPHeaderFlag flags,
    guint * length, guint8 ** header, guint8 ** payload, GstDPVersion version)
{
  guint8 *h;
  guchar *string;
  guint payload_length;

  /* FIXME: GST_IS_CAPS doesn't work
     g_return_val_if_fail (GST_IS_CAPS (caps), FALSE); */
  g_return_val_if_fail (caps, FALSE);
  g_return_val_if_fail (header, FALSE);
  g_return_val_if_fail (payload, FALSE);

  *length = GST_DP_HEADER_LENGTH;
  h = g_malloc0 (GST_DP_HEADER_LENGTH);

  string = (guchar *) gst_caps_to_string (caps);
  payload_length = strlen ((gchar *) string) + 1;       /* include trailing 0 */

  /* version, flags, type */
  GST_DP_INIT_HEADER (h, version, flags, GST_DP_PAYLOAD_CAPS);

  /* buffer properties */
  GST_WRITE_UINT32_BE (h + 6, payload_length);
  GST_WRITE_UINT64_BE (h + 10, (guint64) 0);
  GST_WRITE_UINT64_BE (h + 18, (guint64) 0);
  GST_WRITE_UINT64_BE (h + 26, (guint64) 0);
  GST_WRITE_UINT64_BE (h + 34, (guint64) 0);

  GST_DP_SET_CRC (h, flags, string, payload_length);

  GST_LOG ("created header from caps:");
  gst_dp_dump_byte_array (h, GST_DP_HEADER_LENGTH);
  *header = h;
  *payload = string;
  return TRUE;
}


/*** PUBLIC FUNCTIONS ***/

/**
 * gst_dp_crc:
 *
 * Calculate a CRC for the given buffer over the given number of bytes.
 * This is only provided for verification purposes; typical GDP users
 * will not need this function.
 *
 * Returns: a two-byte CRC checksum.
 */
guint16
gst_dp_crc (const guint8 * buffer, guint length)
{
  static gboolean initialized = FALSE;
  static guint16 crc_table[256];
  guint16 crc_register = CRC_INIT;
  unsigned long i, j, k;

  if (!initialized) {
    for (i = 0; i < 256; i++) {
      j = i << 8;
      for (k = 8; k--;) {
        j = j & 0x8000 ? (j << 1) ^ POLY : j << 1;
      }

      crc_table[i] = (guint16) j;
    }
    initialized = TRUE;
  }

  /* calc CRC */
  for (; length--;) {
    crc_register = (guint16) ((crc_register << 8) ^
        crc_table[((crc_register >> 8) & 0x00ff) ^ *buffer++]);
  }
  return (0xffff ^ crc_register);
}

/* debugging function; dumps byte array values per 8 bytes */
/* FIXME: would be nice to merge this with gst_util_dump_mem () */
void
gst_dp_dump_byte_array (guint8 * array, guint length)
{
  int i;
  int n = 8;                    /* number of bytes per line */
  gchar *line = g_malloc0 (3 * n + 1);

  GST_LOG ("dumping byte array of length %d", length);
  for (i = 0; i < length; ++i) {
    g_sprintf (line + 3 * (i % n), "%02x ", array[i]);
    if (i % n == (n - 1)) {
      GST_LOG ("%03d: %s", i - (n - 1), line);
    }
  }
  if (i % n != 0) {
    GST_LOG ("%03d: %s", (i / n) * n, line);
  }
  g_free (line);
}

GType
gst_dp_version_get_type (void)
{
  static GType gst_dp_version_type = 0;
  static const GEnumValue gst_dp_version[] = {
    {GST_DP_VERSION_0_2, "GDP Version 0.2", "0.2"},
    {GST_DP_VERSION_1_0, "GDP Version 1.0", "1.0"},
    {0, NULL, NULL},
  };

  if (!gst_dp_version_type) {
    gst_dp_version_type =
        g_enum_register_static ("GstDPVersion", gst_dp_version);
  }
  return gst_dp_version_type;
};

/**
 * gst_dp_init:
 *
 * Initialize GStreamer Data Protocol library.
 *
 * Should be called before using these functions from source linking
 * to this source file.
 */
void
gst_dp_init (void)
{
  static gboolean _gst_dp_initialized = FALSE;

  if (_gst_dp_initialized)
    return;

  _gst_dp_initialized = TRUE;

  gst_dp_version_get_type ();

  GST_DEBUG_CATEGORY_INIT (data_protocol_debug, "gdp", 0,
      "GStreamer Data Protocol");
}

/**
 * gst_dp_header_payload_length:
 * @header: the byte header of the packet array
 *
 * Get the length of the payload described by @header.
 *
 * Returns: the length of the payload this header describes.
 */
guint32
gst_dp_header_payload_length (const guint8 * header)
{
  return GST_DP_HEADER_PAYLOAD_LENGTH (header);
}

/**
 * gst_dp_header_payload_type:
 * @header: the byte header of the packet array
 *
 * Get the type of the payload described by @header.
 *
 * Returns: the #GstDPPayloadType the payload this header describes.
 */
GstDPPayloadType
gst_dp_header_payload_type (const guint8 * header)
{
  return GST_DP_HEADER_PAYLOAD_TYPE (header);
}

/*** PACKETIZER FUNCTIONS ***/

/**
 * gst_dp_header_from_buffer:
 * @buffer: a #GstBuffer to create a header for
 * @flags: the #GDPHeaderFlags to create the header with
 * @length: a guint pointer to store the header length in
 * @header: a guint8 * pointer to store a newly allocated header byte array in
 *
 * Creates a GDP header from the given buffer.
 *
 * Deprecated: use a #GstDPPacketizer
 *
 * Returns: %TRUE if the header was successfully created.
 */
gboolean
gst_dp_header_from_buffer (const GstBuffer * buffer, GstDPHeaderFlag flags,
    guint * length, guint8 ** header)
{
  return gst_dp_header_from_buffer_any (buffer, flags, length, header,
      GST_DP_VERSION_0_2);
}

static gboolean
gst_dp_header_from_buffer_1_0 (const GstBuffer * buffer, GstDPHeaderFlag flags,
    guint * length, guint8 ** header)
{
  return gst_dp_header_from_buffer_any (buffer, flags, length, header,
      GST_DP_VERSION_1_0);
}

 /**
 * gst_dp_packet_from_caps:
 * @caps: a #GstCaps to create a packet for
 * @flags: the #GDPHeaderFlags to create the header with
 * @length: a guint pointer to store the header length in
 * @header: a guint8 pointer to store a newly allocated header byte array in
 * @payload: a guint8 pointer to store a newly allocated payload byte array in
 *
 * Creates a GDP packet from the given caps.
 *
 * Deprecated: use a #GstDPPacketizer
 *
 * Returns: %TRUE if the packet was successfully created.
 */
gboolean
gst_dp_packet_from_caps (const GstCaps * caps, GstDPHeaderFlag flags,
    guint * length, guint8 ** header, guint8 ** payload)
{
  return gst_dp_packet_from_caps_any (caps, flags, length, header, payload,
      GST_DP_VERSION_0_2);
}

gboolean
gst_dp_packet_from_caps_1_0 (const GstCaps * caps, GstDPHeaderFlag flags,
    guint * length, guint8 ** header, guint8 ** payload)
{
  return gst_dp_packet_from_caps_any (caps, flags, length, header, payload,
      GST_DP_VERSION_1_0);
}

/**
 * gst_dp_packet_from_event:
 * @event: a #GstEvent to create a packet for
 * @flags: the #GDPHeaderFlags to create the header with
 * @length: a guint pointer to store the header length in
 * @header: a guint8 pointer to store a newly allocated header byte array in
 * @payload: a guint8 pointer to store a newly allocated payload byte array in
 *
 * Creates a GDP packet from the given event.
 *
 * Deprecated: use a #GstDPPacketizer
 *
 * Returns: %TRUE if the packet was successfully created.
 */
gboolean
gst_dp_packet_from_event (const GstEvent * event, GstDPHeaderFlag flags,
    guint * length, guint8 ** header, guint8 ** payload)
{
  guint8 *h;
  guint pl_length;              /* length of payload */

  g_return_val_if_fail (event, FALSE);
  g_return_val_if_fail (GST_IS_EVENT (event), FALSE);
  g_return_val_if_fail (header, FALSE);
  g_return_val_if_fail (payload, FALSE);

  *length = GST_DP_HEADER_LENGTH;
  h = g_malloc0 (GST_DP_HEADER_LENGTH);

  /* first construct payload, since we need the length */
  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_UNKNOWN:
      GST_WARNING ("Unknown event, ignoring");
      *length = 0;
      g_free (h);
      return FALSE;
    case GST_EVENT_EOS:
    case GST_EVENT_FLUSH_START:
    case GST_EVENT_FLUSH_STOP:
    case GST_EVENT_NEWSEGMENT:
      pl_length = 0;
      *payload = NULL;
      break;
    case GST_EVENT_SEEK:
    {
      gdouble rate;
      GstFormat format;
      GstSeekFlags flags;
      GstSeekType cur_type, stop_type;
      gint64 cur, stop;

      gst_event_parse_seek ((GstEvent *) event, &rate, &format, &flags,
          &cur_type, &cur, &stop_type, &stop);

      pl_length = 4 + 4 + 4 + 8 + 4 + 8;
      *payload = g_malloc0 (pl_length);
      /* FIXME write rate */
      GST_WRITE_UINT32_BE (*payload, (guint32) format);
      GST_WRITE_UINT32_BE (*payload + 4, (guint32) flags);
      GST_WRITE_UINT32_BE (*payload + 8, (guint32) cur_type);
      GST_WRITE_UINT64_BE (*payload + 12, (guint64) cur);
      GST_WRITE_UINT32_BE (*payload + 20, (guint32) stop_type);
      GST_WRITE_UINT64_BE (*payload + 24, (guint64) stop);
      break;
    }
    case GST_EVENT_QOS:
    case GST_EVENT_NAVIGATION:
    case GST_EVENT_TAG:
      GST_WARNING ("Unhandled event type %d, ignoring", GST_EVENT_TYPE (event));
      *length = 0;
      g_free (h);
      return FALSE;
    default:
      GST_WARNING ("Unknown event type %d, ignoring", GST_EVENT_TYPE (event));
      *length = 0;
      g_free (h);
      return FALSE;
  }

  /* version, flags, type */
  GST_DP_INIT_HEADER (h, GST_DP_VERSION_0_2, flags,
      GST_DP_PAYLOAD_EVENT_NONE + GST_EVENT_TYPE (event));

  /* length */
  GST_WRITE_UINT32_BE (h + 6, (guint32) pl_length);
  /* timestamp */
  GST_WRITE_UINT64_BE (h + 10, GST_EVENT_TIMESTAMP (event));

  GST_DP_SET_CRC (h, flags, *payload, pl_length);

  GST_LOG ("created header from event:");
  gst_dp_dump_byte_array (h, GST_DP_HEADER_LENGTH);
  *header = h;
  return TRUE;
}

static gboolean
gst_dp_packet_from_event_1_0 (const GstEvent * event, GstDPHeaderFlag flags,
    guint * length, guint8 ** header, guint8 ** payload)
{
  guint8 *h;
  guint32 pl_length;            /* length of payload */
  guchar *string = NULL;

  g_return_val_if_fail (event, FALSE);
  g_return_val_if_fail (GST_IS_EVENT (event), FALSE);
  g_return_val_if_fail (header, FALSE);
  g_return_val_if_fail (payload, FALSE);

  *length = GST_DP_HEADER_LENGTH;
  h = g_malloc0 (GST_DP_HEADER_LENGTH);

  if (event->structure) {
    string = (guchar *) gst_structure_to_string (event->structure);
    GST_LOG ("event %p has structure, string %s", event, string);
    pl_length = strlen ((gchar *) string) + 1;  /* include trailing 0 */
  } else {
    GST_LOG ("event %p has no structure");
    pl_length = 0;
  }

  /* version, flags, type */
  GST_DP_INIT_HEADER (h, GST_DP_VERSION_1_0, flags,
      GST_DP_PAYLOAD_EVENT_NONE + GST_EVENT_TYPE (event));

  /* length */
  GST_WRITE_UINT32_BE (h + 6, pl_length);
  /* timestamp */
  GST_WRITE_UINT64_BE (h + 10, GST_EVENT_TIMESTAMP (event));

  GST_DP_SET_CRC (h, flags, *payload, pl_length);

  GST_LOG ("created header from event:");
  gst_dp_dump_byte_array (h, GST_DP_HEADER_LENGTH);
  *header = h;
  *payload = string;
  return TRUE;
}

/*** DEPACKETIZING FUNCTIONS ***/

/**
 * gst_dp_buffer_from_header:
 * @header_length: the length of the packet header
 * @header: the byte array of the packet header
 *
 * Creates a newly allocated #GstBuffer from the given header.
 * The buffer data needs to be copied into it before validating.
 *
 * Use this function if you want to pre-allocate a buffer based on the
 * packet header to read the packet payload in to.
 *
 * Returns: A #GstBuffer if the buffer was successfully created, or NULL.
 */
GstBuffer *
gst_dp_buffer_from_header (guint header_length, const guint8 * header)
{
  GstBuffer *buffer;

  g_return_val_if_fail (GST_DP_HEADER_PAYLOAD_TYPE (header) ==
      GST_DP_PAYLOAD_BUFFER, NULL);
  buffer =
      gst_buffer_new_and_alloc ((guint) GST_DP_HEADER_PAYLOAD_LENGTH (header));
  GST_BUFFER_TIMESTAMP (buffer) = GST_DP_HEADER_TIMESTAMP (header);
  GST_BUFFER_DURATION (buffer) = GST_DP_HEADER_DURATION (header);
  GST_BUFFER_OFFSET (buffer) = GST_DP_HEADER_OFFSET (header);
  GST_BUFFER_OFFSET_END (buffer) = GST_DP_HEADER_OFFSET_END (header);
  GST_BUFFER_FLAGS (buffer) = GST_DP_HEADER_BUFFER_FLAGS (header);

  return buffer;
}

/**
 * gst_dp_caps_from_packet:
 * @header_length: the length of the packet header
 * @header: the byte array of the packet header
 * @payload: the byte array of the packet payload
 *
 * Creates a newly allocated #GstCaps from the given packet.
 *
 * Returns: A #GstCaps containing the caps represented in the packet,
 *          or NULL if the packet could not be converted.
 */
GstCaps *
gst_dp_caps_from_packet (guint header_length, const guint8 * header,
    const guint8 * payload)
{
  GstCaps *caps;
  gchar *string;

  g_return_val_if_fail (header, NULL);
  g_return_val_if_fail (payload, NULL);
  g_return_val_if_fail (GST_DP_HEADER_PAYLOAD_TYPE (header) ==
      GST_DP_PAYLOAD_CAPS, NULL);

  string = g_strndup ((gchar *) payload, GST_DP_HEADER_PAYLOAD_LENGTH (header));
  caps = gst_caps_from_string (string);
  g_free (string);
  return caps;
}

static GstEvent *
gst_dp_event_from_packet_0_2 (guint header_length, const guint8 * header,
    const guint8 * payload)
{
  GstEvent *event = NULL;
  GstEventType type;

  type = GST_DP_HEADER_PAYLOAD_TYPE (header) - GST_DP_PAYLOAD_EVENT_NONE;
  switch (type) {
    case GST_EVENT_UNKNOWN:
      GST_WARNING ("Unknown event, ignoring");
      return FALSE;
    case GST_EVENT_EOS:
    case GST_EVENT_FLUSH_START:
    case GST_EVENT_FLUSH_STOP:
    case GST_EVENT_NEWSEGMENT:
      event = gst_event_new_custom (type, NULL);
      GST_EVENT_TIMESTAMP (event) = GST_DP_HEADER_TIMESTAMP (header);
      break;
    case GST_EVENT_SEEK:
    {
      gdouble rate;
      GstFormat format;
      GstSeekFlags flags;
      GstSeekType cur_type, stop_type;
      gint64 cur, stop;

      /* FIXME, read rate */
      rate = 1.0;
      format = (GstFormat) GST_READ_UINT32_BE (payload);
      flags = (GstSeekFlags) GST_READ_UINT32_BE (payload + 4);
      cur_type = (GstSeekType) GST_READ_UINT32_BE (payload + 8);
      cur = (gint64) GST_READ_UINT64_BE (payload + 12);
      stop_type = (GstSeekType) GST_READ_UINT32_BE (payload + 20);
      stop = (gint64) GST_READ_UINT64_BE (payload + 24);

      event = gst_event_new_seek (rate, format, flags, cur_type, cur,
          stop_type, stop);
      GST_EVENT_TIMESTAMP (event) = GST_DP_HEADER_TIMESTAMP (header);
      break;
    }
    case GST_EVENT_QOS:
    case GST_EVENT_NAVIGATION:
    case GST_EVENT_TAG:
      GST_WARNING ("Unhandled event type %d, ignoring", type);
      return FALSE;
    default:
      GST_WARNING ("Unknown event type %d, ignoring", type);
      return FALSE;
  }

  return event;
}

static GstEvent *
gst_dp_event_from_packet_1_0 (guint header_length, const guint8 * header,
    const guint8 * payload)
{
  GstEvent *event = NULL;
  GstEventType type;
  gchar *string;
  GstStructure *s;

  type = GST_DP_HEADER_PAYLOAD_TYPE (header) - GST_DP_PAYLOAD_EVENT_NONE;
  string = g_strndup ((gchar *) payload, GST_DP_HEADER_PAYLOAD_LENGTH (header));
  s = gst_structure_from_string (string, NULL);
  g_free (string);
  if (!s)
    return NULL;
  event = gst_event_new_custom (type, s);
  return event;
}


/**
 * gst_dp_event_from_packet:
 * @header_length: the length of the packet header
 * @header: the byte array of the packet header
 * @payload: the byte array of the packet payload
 *
 * Creates a newly allocated #GstEvent from the given packet.
 *
 * Returns: A #GstEvent if the event was successfully created,
 *          or NULL if an event could not be read from the payload.
 */
GstEvent *
gst_dp_event_from_packet (guint header_length, const guint8 * header,
    const guint8 * payload)
{
  guint8 major, minor;

  g_return_val_if_fail (header, NULL);

  major = GST_DP_HEADER_MAJOR_VERSION (header);
  minor = GST_DP_HEADER_MINOR_VERSION (header);

  if (major == 0 && minor == 2)
    return gst_dp_event_from_packet_0_2 (header_length, header, payload);
  else if (major == 1 && minor == 0)
    return gst_dp_event_from_packet_1_0 (header_length, header, payload);
  else {
    GST_ERROR ("Unknown GDP version %d.%d", major, minor);
    return NULL;
  }
}

/**
 * gst_dp_validate_header:
 * @header_length: the length of the packet header
 * @header: the byte array of the packet header
 *
 * Validates the given packet header by checking the CRC checksum.
 *
 * Returns: %TRUE if the CRC matches, or no CRC checksum is present.
 */
gboolean
gst_dp_validate_header (guint header_length, const guint8 * header)
{
  guint16 crc_read, crc_calculated;

  if (!(GST_DP_HEADER_FLAGS (header) & GST_DP_HEADER_FLAG_CRC_HEADER))
    return TRUE;
  crc_read = GST_DP_HEADER_CRC_HEADER (header);
  /* don't include the last two crc fields for the crc check */
  crc_calculated = gst_dp_crc (header, header_length - 4);
  if (crc_read != crc_calculated) {
    GST_WARNING ("header crc mismatch: read %02x, calculated %02x", crc_read,
        crc_calculated);
    return FALSE;
  }
  GST_LOG ("header crc validation: %02x", crc_read);
  return TRUE;
}

/**
 * gst_dp_validate_payload:
 * @header_length: the length of the packet header
 * @header: the byte array of the packet header
 * @payload: the byte array of the packet payload
 *
 * Validates the given packet payload using the given packet header
 * by checking the CRC checksum.
 *
 * Returns: %TRUE if the CRC matches, or no CRC checksum is present.
 */
gboolean
gst_dp_validate_payload (guint header_length, const guint8 * header,
    const guint8 * payload)
{
  guint16 crc_read, crc_calculated;

  if (!(GST_DP_HEADER_FLAGS (header) & GST_DP_HEADER_FLAG_CRC_PAYLOAD))
    return TRUE;
  crc_read = GST_DP_HEADER_CRC_PAYLOAD (header);
  crc_calculated = gst_dp_crc (payload, GST_DP_HEADER_PAYLOAD_LENGTH (header));
  if (crc_read != crc_calculated) {
    GST_WARNING ("payload crc mismatch: read %02x, calculated %02x", crc_read,
        crc_calculated);
    return FALSE;
  }
  GST_LOG ("payload crc validation: %02x", crc_read);
  return TRUE;
}

/**
 * gst_dp_validate_packet:
 * @header_length: the length of the packet header
 * @header: the byte array of the packet header
 * @payload: the byte array of the packet payload
 *
 * Validates the given packet by checking version information and checksums.
 *
 * Returns: %TRUE if the packet validates.
 */
gboolean
gst_dp_validate_packet (guint header_length, const guint8 * header,
    const guint8 * payload)
{
  if (!gst_dp_validate_header (header_length, header))
    return FALSE;
  if (!gst_dp_validate_payload (header_length, header, payload))
    return FALSE;

  return TRUE;
}

/**
 * gst_dp_packetizer_new:
 * @version: the #GstDPVersion of the protocol to packetize for.
 *
 * Creates a new packetizer.
 *
 * Returns: a newly allocated #GstDPPacketizer
 */
GstDPPacketizer *
gst_dp_packetizer_new (GstDPVersion version)
{
  GstDPPacketizer *ret;

  ret = g_malloc0 (sizeof (GstDPPacketizer));
  ret->version = version;

  switch (version) {
    case GST_DP_VERSION_0_2:
      ret->header_from_buffer = gst_dp_header_from_buffer;
      ret->packet_from_caps = gst_dp_packet_from_caps;
      ret->packet_from_event = gst_dp_packet_from_event;
      break;
    case GST_DP_VERSION_1_0:
      ret->header_from_buffer = gst_dp_header_from_buffer_1_0;
      ret->packet_from_caps = gst_dp_packet_from_caps_1_0;
      ret->packet_from_event = gst_dp_packet_from_event_1_0;
      break;
    default:
      g_free (ret);
      ret = NULL;
      break;
  }

  return ret;
}

/**
 * gst_dp_packetizer_free:
 * @packetizer: the #GstDPPacketizer to free.
 *
 * Free the given packetizer.
 */
void
gst_dp_packetizer_free (GstDPPacketizer * packetizer)
{
  g_free (packetizer);
}