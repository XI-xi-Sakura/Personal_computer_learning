/* Copyright (c) 2000, 2025, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   Without limiting anything contained in the foregoing, this file,
   which is part of C Driver for MySQL (Connector/C), is also subject to the
   Universal FOSS Exception, version 1.0, a copy of which can be found at
   http://oss.oracle.com/licenses/universal-foss-exception.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file

  This file is the net layer API for the MySQL client/server protocol.

  Write and read of logical packets to/from socket.

  Writes are cached into net_buffer_length big packets.
  Read packets are reallocated dynamically when reading big packets.
  Each logical packet has the following pre-info:
  3 byte length & 1 byte package-number.
*/

#include <string.h>
#include <sys/types.h>
#include <algorithm>

#include <mysql/components/services/log_builtins.h>
#include <mysql/thread_pool_priv.h>
#include "../sql/current_thd.h"
#include "../sql/sql_class.h"
#include "../sql/sql_thd_internal_api.h"
#include "my_byteorder.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_io.h"
#include "my_macros.h"
#include "my_sys.h"
#include "mysql.h"
#include "mysql/service_mysql_alloc.h"
#include "mysql_async.h"
#include "mysql_com.h"
#include "mysqld_error.h"
#include "violite.h"

using std::max;
using std::min;

#ifdef MYSQL_SERVER
#include "sql/psi_memory_key.h"
#else
#define key_memory_NET_buff 0
#define key_memory_NET_compress_packet 0
#endif

/*
  The following handles the differences when this is linked between the
  client and the server.

  This gives an error if a too big packet is found.
  The server can change this, but because the client can't normally do this
  the client should have a bigger max_allowed_packet.
*/

#ifdef MYSQL_SERVER
/*
  The following variables/functions should really not be declared
  extern, but as it's hard to include sql_class.h here, we have to
  live with this for a while.
*/
extern void thd_increment_bytes_sent(size_t length);
extern void thd_increment_bytes_received(size_t length);

/* Additional instrumentation hooks for the server */
#include "mysql_com_server.h"

/* Refresh cached values in the THD for the server. */
static void server_store_cached_values(const NET *net) {
  // Refresh only if this net is the net of the current_thd
  if (current_thd != nullptr && current_thd->get_net() == net)
    current_thd->store_cached_properties(THD::cached_properties::RW_STATUS);
}
#endif

static bool net_write_buff(NET *, const uchar *, size_t);
static uchar *compress_packet(NET *net, const uchar *packet, size_t *length);

NET_EXTENSION *net_extension_init() {
  NET_EXTENSION *ext = static_cast<NET_EXTENSION *>(my_malloc(
      PSI_NOT_INSTRUMENTED, sizeof(NET_EXTENSION), MYF(MY_WME | MY_ZEROFILL)));
  ext->net_async_context = static_cast<NET_ASYNC *>(my_malloc(
      PSI_NOT_INSTRUMENTED, sizeof(NET_ASYNC), MYF(MY_WME | MY_ZEROFILL)));
  ext->compress_ctx.algorithm = enum_compression_algorithm::MYSQL_UNCOMPRESSED;
  return ext;
}

void net_extension_free(NET *net) {
  NET_EXTENSION *ext = NET_EXTENSION_PTR(net);
  if (ext) {
#ifndef MYSQL_SERVER
    if (ext->net_async_context) {
      my_free(ext->net_async_context);
      ext->net_async_context = nullptr;
    }
    mysql_compress_context_deinit(&ext->compress_ctx);
    my_free(ext);
    net->extension = nullptr;
#endif
  }
}

/**
  Returns the appropriate compression_context based on caller.
  If the caller is server then fetch is from the server extension
  structure.

  @param[in] net    NET structure

  @returns mysql_compress_context structure pointer
*/
static mysql_compress_context *compress_context(NET *net) {
  mysql_compress_context *mysql_compress_ctx = nullptr;
#ifdef MYSQL_SERVER
  NET_SERVER *server_extension = static_cast<NET_SERVER *>(net->extension);
  if (server_extension != nullptr) {
    mysql_compress_ctx = &server_extension->compress_ctx;
  }
#else
  NET_EXTENSION *ext = NET_EXTENSION_PTR(net);
  if (ext != nullptr) mysql_compress_ctx = &ext->compress_ctx;
#endif
  return mysql_compress_ctx;
}

/** Init with packet info. */

bool my_net_init(NET *net, Vio *vio) {
  DBUG_TRACE;
  net->vio = vio;
  my_net_local_init(net); /* Set some limits */
  if (!(net->buff = (uchar *)my_malloc(
            key_memory_NET_buff,
            (size_t)net->max_packet + NET_HEADER_SIZE + COMP_HEADER_SIZE,
            MYF(MY_WME))))
    return true;
  net->buff_end = net->buff + net->max_packet;
  net->error = NET_ERROR_UNSET;
  net->return_status = nullptr;
  net->pkt_nr = net->compress_pkt_nr = 0;
  net->write_pos = net->read_pos = net->buff;
  net->last_error[0] = 0;
  net->compress = false;
  net->reading_or_writing = 0;
  net->where_b = net->remain_in_buf = 0;
  net->last_errno = 0;
#ifdef MYSQL_SERVER
  net->extension = nullptr;
  server_store_cached_values(net);
#else
  NET_EXTENSION *ext = net_extension_init();
  ext->net_async_context->cur_pos = net->buff + net->where_b;
  ext->net_async_context->read_rows_is_first_read = true;
  ext->net_async_context->async_operation = NET_ASYNC_OP_IDLE;
  ext->net_async_context->async_send_command_status =
      NET_ASYNC_SEND_COMMAND_IDLE;
  ext->net_async_context->async_read_query_result_status =
      NET_ASYNC_READ_QUERY_RESULT_IDLE;
  ext->net_async_context->async_packet_read_state = NET_ASYNC_PACKET_READ_IDLE;
  ext->compress_ctx.algorithm = enum_compression_algorithm::MYSQL_UNCOMPRESSED;
  net->extension = ext;
#endif
  if (vio) {
    /* For perl DBI/DBD. */
    net->fd = vio_fd(vio);
    vio_fastsend(vio);
  }
  return false;
}

void net_end(NET *net) {
  DBUG_TRACE;
#ifdef MYSQL_SERVER
  NET_SERVER *server_extension = static_cast<NET_SERVER *>(net->extension);
  if (server_extension != nullptr)
    mysql_compress_context_deinit(&server_extension->compress_ctx);
#else
  net_extension_free(net);
#endif
  my_free(net->buff);
  net->buff = nullptr;
}

void net_claim_memory_ownership(NET *net, bool claim) {
  my_claim(net->buff, claim);
}

/** Realloc the packet buffer. */

bool net_realloc(NET *net, size_t length) {
  uchar *buff;
  size_t pkt_length;
  DBUG_TRACE;
  DBUG_PRINT("enter", ("length: %lu", (ulong)length));

  if (length >= net->max_packet_size) {
    DBUG_PRINT("error",
               ("Packet too large. Max size: %lu", net->max_packet_size));
    /* Error, but no need to stop using the socket. */
    net->error = NET_ERROR_SOCKET_RECOVERABLE;
    net->last_errno = ER_NET_PACKET_TOO_LARGE;
#ifdef MYSQL_SERVER
    my_error(ER_NET_PACKET_TOO_LARGE, MYF(0));
#endif
    return true;
  }
  pkt_length = (length + IO_SIZE - 1) & ~(IO_SIZE - 1);
  /*
    We must allocate some extra bytes for the end 0 and to be able to
    read big compressed blocks in
    net_read_packet() may actually read 4 bytes depending on build flags and
    platform.
  */
  if (!(buff = (uchar *)my_realloc(
            key_memory_NET_buff, (char *)net->buff,
            pkt_length + NET_HEADER_SIZE + COMP_HEADER_SIZE, MYF(MY_WME)))) {
    /* Error, but no need to stop using the socket. */
    net->error = NET_ERROR_SOCKET_RECOVERABLE;
    net->last_errno = ER_OUT_OF_RESOURCES;
    /* In the server the error is reported by MY_WME flag. */
    return true;
  }
#ifdef MYSQL_SERVER
  net->buff = net->write_pos = buff;
#else
  const size_t cur_pos_offset = NET_ASYNC_DATA(net)->cur_pos - net->buff;
  net->buff = net->write_pos = buff;
  NET_ASYNC_DATA(net)->cur_pos = net->buff + cur_pos_offset;
#endif
  net->buff_end = buff + (net->max_packet = (ulong)pkt_length);
  return false;
}

/**
  Clear (reinitialize) the NET structure for a new command.

  @remark Performs debug checking of the socket buffer to
          ensure that the protocol sequence is correct.

  @param net          NET handler
  @param check_buffer  Whether to check the socket buffer.
*/

void net_clear(NET *net, bool check_buffer [[maybe_unused]]) {
  DBUG_TRACE;

  DBUG_EXECUTE_IF("simulate_bad_field_length_1", {
    net->pkt_nr = net->compress_pkt_nr = 0;
    net->write_pos = net->buff;
    return;
  });
  DBUG_EXECUTE_IF("simulate_bad_field_length_2", {
    net->pkt_nr = net->compress_pkt_nr = 0;
    net->write_pos = net->buff;
    return;
  });

  /* Ready for new command */
  net->pkt_nr = net->compress_pkt_nr = 0;
  net->write_pos = net->buff;
}

/** Flush write_buffer if not empty. */

bool net_flush(NET *net) {
  bool error = false;
  DBUG_TRACE;
  if (net->buff != net->write_pos) {
    error =
        net_write_packet(net, net->buff, (size_t)(net->write_pos - net->buff));
    net->write_pos = net->buff;
  }
  /* Sync packet number if using compression */
  if (net->compress) net->pkt_nr = net->compress_pkt_nr;
  return error;
}

/**
  Whether a I/O operation should be retried later.

  @param net          NET handler.
  @param retry_count  Maximum number of interrupted operations.

  @retval true    Operation should be retried.
  @retval false   Operation should not be retried. Fatal error.
*/

static bool net_should_retry(NET *net, uint *retry_count [[maybe_unused]]) {
  bool retry;

#ifndef MYSQL_SERVER
  /*
    In the  client library, interrupted I/O operations are always retried.
    Otherwise, it's either a timeout or an unrecoverable error.
  */
  retry = vio_should_retry(net->vio);
#else
  /*
    In the server, interrupted I/O operations are retried up to a limit.
    In this scenario, pthread_kill can be used to wake up
    (interrupt) threads waiting for I/O.
  */
  retry = vio_should_retry(net->vio) && ((*retry_count)++ < net->retry_count);
#endif

  return retry;
}

/* clang-format off */
/**
  @page page_protocol_basic_packets MySQL Packets

  If a MySQL client or server wants to send data, it:
  - Splits the data into packets of size 2<sup>24</sup> bytes
  - Prepends to each chunk a packet header

  @section sect_protocol_basic_packets_packet Protocol::Packet

  Data between client and server is exchanged in packets of max 16MByte size.

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;3&gt;"</td>
      <td>payload_length</td>
      <td>Length of the payload. The number of bytes in the packet beyond
          the initial 4 bytes that make up the packet header.</td></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
      <td>sequence_id</td>
      <td>@ref sect_protocol_basic_packets_sequence_id</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_var "string&lt;var&gt;"</td>
      <td>payload</td>
      <td>payload of the packet</td></tr>
  </table>

  Example:

  @todo: Reference COM_QUIT
  A COM_QUIT looks like this:
  <table><tr>
  <td>
  ~~~~~~~~~~~~~~~~~~~~~
  01 00 00 00 01
  ~~~~~~~~~~~~~~~~~~~~~
  </td><td>
  - length: 1
  - sequence_id: x00
  - payload: 0x01
  </td></tr></table>

  @sa my_net_write(), net_write_command(), net_write_buff(), my_net_read(),
  net_send_ok()

  @section sect_protocol_basic_packets_sending_mt_16mb Sending More Than 16Mb

  If the payload is larger than or equal to 2<sup>24</sup>-1 bytes the length
  is set to 2<sup>24</sup>-1 (`ff ff ff`) and a additional packets are sent
  with the rest of the payload until the payload of a packet is less
  than 2<sup>24</sup>-1 bytes.

  Sending a payload of 16 777 215 (2<sup>24</sup>-1) bytes looks like:

  ~~~~~~~~~~~~~~~~
  ff ff ff 00 ...
  00 00 00 01
  ~~~~~~~~~~~~~~~~

  @section sect_protocol_basic_packets_sequence_id Sequence ID

  The sequence-id is incremented with each packet and may wrap around.
  It starts at 0 and is reset to 0 when a new command begins in the
  @ref page_protocol_command_phase.

  @section sect_protocol_basic_packets_describing_packets Describing Packets

  In this document we describe each packet by first defining its payload and
  provide an example showing each packet that is sent, including its packet
  header:
  <pre>
  &lt;packetname&gt;
    &lt;description&gt;

    direction: client -&gt; server
    response: &lt;response&gt;

    payload:
      &lt;type&gt;        &lt;description&gt;
  </pre>

  Example:
  ~~~~~~~~~~~~~~~~~~~~~
  01 00 00 00 01
  ~~~~~~~~~~~~~~~~~~~~~

  @note Some packets have optional fields or a different layout depending on
  the @ref group_cs_capabilities_flags.

  If a field has a fixed value, its description shows it as a hex value in
  brackets like this: `[00]`
*/
/* clang-format on */

/*****************************************************************************
** Write something to server/client buffer
*****************************************************************************/

/**
  Write a logical packet with packet header.

  Format: Packet length (3 bytes), packet number (1 byte)
  When compression is used, a 3 byte compression length is added.

  @note If compression is used, the original packet is modified!
*/

bool my_net_write(NET *net, const uchar *packet, size_t len) {
  uchar buff[NET_HEADER_SIZE];

  DBUG_DUMP("net write", packet, len);

  if (unlikely(!net->vio)) /* nowhere to write */
    return false;

  DBUG_EXECUTE_IF("simulate_net_write_failure", {
    my_error(ER_NET_ERROR_ON_WRITE, MYF(0));
    return 1;
  };);

  /* turn off non blocking operations */
  if (!vio_is_blocking(net->vio)) vio_set_blocking_flag(net->vio, true);
  /*
    Big packets are handled by splitting them in packets of MAX_PACKET_LENGTH
    length. The last packet is always a packet that is < MAX_PACKET_LENGTH.
    (The last packet may even have a length of 0)
  */
  while (len >= MAX_PACKET_LENGTH) {
    const ulong z_size = MAX_PACKET_LENGTH;
    int3store(buff, z_size);
    buff[3] = (uchar)net->pkt_nr++;
    if (net_write_buff(net, buff, NET_HEADER_SIZE) ||
        net_write_buff(net, packet, z_size)) {
      return true;
    }
    packet += z_size;
    len -= z_size;
  }
  /* Write last packet */
  int3store(buff, static_cast<uint>(len));
  buff[3] = (uchar)net->pkt_nr++;
  if (net_write_buff(net, buff, NET_HEADER_SIZE)) {
    return true;
  }
#ifdef DEBUG_DATA_PACKETS
  DBUG_DUMP("packet_header", buff, NET_HEADER_SIZE);
#endif
  return net_write_buff(net, packet, len);
}

static void reset_packet_write_state(NET *net) {
  DBUG_TRACE;
  NET_ASYNC *net_async = NET_ASYNC_DATA(net);
  if (net_async->async_write_vector) {
    if (net_async->async_write_vector != net_async->inline_async_write_vector) {
      my_free(net_async->async_write_vector);
    }
    net_async->async_write_vector = nullptr;
  }

  if (net_async->async_write_headers) {
    if (net_async->async_write_headers !=
        net_async->inline_async_write_header) {
      my_free(net_async->async_write_headers);
    }
    net_async->async_write_headers = nullptr;
  }

  net_async->async_write_vector_size = 0;
  net_async->async_write_vector_current = 0;
  if (net_async->compressed_write_buffers) {
    /*
      There are two entries per packet, one for header and one for payload.
      We only need to free payloads as headers have their own buffer. If the
      last packet was size 0, the vector size will be 1 lower and due to int
      truncation for odd numbers will be correctly accounted for.
    */
    for (size_t i = 0; i < net_async->compressed_buffers_size; ++i) {
      my_free(net_async->compressed_write_buffers[i]);
    }
    my_free(net_async->compressed_write_buffers);
    net_async->compressed_write_buffers = nullptr;
    net_async->compressed_buffers_size = 0;
  }
}

/*
   Construct the proper buffers for our nonblocking write.  What we do
   here is we make an iovector for the entire write (header, command,
   and payload).  We then continually call writev on this vector,
   consuming parts from it as bytes are successfully written.  Headers
   for the message are all stored inside one buffer, separate from the
   payload; this lets us avoid copying the entire query just to insert
   the headers every 2**24 bytes.

   The most common case is the query fits in a packet.  In that case,
   we don't construct the iovector dynamically, instead using one we
   pre-allocated inside the net structure.  This avoids allocations in
   the common path, but requires special casing with our iovec and
   header buffer.
*/
static int begin_packet_write_state(NET *net, uchar command,
                                    const uchar *packet, size_t packet_len,
                                    const uchar *optional_prefix,
                                    size_t prefix_len) {
  DBUG_TRACE;
  size_t header_len = NET_HEADER_SIZE;
  if (net->compress) {
    header_len += NET_HEADER_SIZE + COMP_HEADER_SIZE;
  }
  NET_ASYNC *net_async = NET_ASYNC_DATA(net);
  size_t total_len = packet_len + prefix_len;
  const bool include_command = (command < COM_END);
  if (include_command) {
    ++total_len;
  }
  const size_t packet_count = 1 + total_len / MAX_PACKET_LENGTH;
  reset_packet_write_state(net);

  struct io_vec *vec;
  uchar *headers;
  uchar **compressed_buffers = nullptr;
  if (total_len < MAX_PACKET_LENGTH) {
    /*
      Most writes hit this case, ie, less than MAX_PACKET_LENGTH of
      query text.
    */
    vec = net_async->inline_async_write_vector;
    headers = net_async->inline_async_write_header;
  } else {
    /* Large query, create the vector and header buffer dynamically. */
    vec = (struct io_vec *)my_malloc(
        PSI_NOT_INSTRUMENTED, sizeof(struct io_vec) * (packet_count * 2 + 1),
        MYF(MY_ZEROFILL));
    if (!vec) {
      return 0;
    }

    headers =
        (uchar *)my_malloc(PSI_NOT_INSTRUMENTED,
                           packet_count * (header_len + 1), MYF(MY_ZEROFILL));
    if (!headers) {
      my_free(vec);
      return 0;
    }
  }
  /*
    Regardless of where vec and headers come from, these are what we
    feed to writev and populate below.
  */
  net_async->async_write_vector = vec;
  net_async->async_write_headers = headers;
  if (net->compress) {
    // Will need to hand compress and manage at most 1 buffer per packet
    compressed_buffers =
        (uchar **)my_malloc(key_memory_NET_compress_packet,
                            sizeof(uchar *) * packet_count, MYF(MY_ZEROFILL));
    if (!compressed_buffers) {
      reset_packet_write_state(net);
      return 0;
    }
  }
  net_async->compressed_write_buffers = compressed_buffers;

  /*
    We sneak the command into the first header, so the special casing
    below about packet_num == 0 relates to that.  This lets us avoid
    an extra allocation and copying the input buffers again.

    Every chunk of MAX_PACKET_LENGTH results in a header and a
    payload, so we have twice as many entries in the IO
    vector as we have packet_count.  The first packet may be prefixed with a
    small amount of data, so that one actually might
    consume *three* iovec entries.
  */
  for (size_t packet_num = 0; packet_num < packet_count; ++packet_num) {
    /*
      The first iovec contains the headers only and command if it is provided
    */
    uchar *buf = headers + packet_num * (header_len + 1);
    size_t bytes_queued = 0;
    (*vec).iov_base = buf;
    (*vec).iov_len = header_len;

    /*
     if using compression, add the compression header. Usually, we would rely on
     compress_packet to add compression headers, but here we assume
     that headers do not compress well due to their short length and send them
     as is by constructing our own packat and incrementing compress_pkt_nr
     manually.

     We don't compress the headers together with the payload because that
     would mean extra memcpy's to concatenate the buffers to pass into
     compress_packet.
    */
    if (net->compress) {
      size_t comp_packet_len = NET_HEADER_SIZE;
      if (packet_num == 0) {
        comp_packet_len += prefix_len + (include_command ? 1 : 0);
      }
      int3store(buf, comp_packet_len);
      buf[3] = (uchar)net->compress_pkt_nr++;
      /*
       The bytes in COMP_HEADER_SIZE are implicitly zero because they were
       zero filled. A zero length means that the contents are uncompressed.
      */
      buf += NET_HEADER_SIZE + COMP_HEADER_SIZE;
    }

    size_t packet_size = min<size_t>(MAX_PACKET_LENGTH, total_len);
    int3store(buf, packet_size);
    buf[3] = (uchar)net->pkt_nr++;
    /*
      We sneak the command byte into the header, even though
      technically it is payload.  This lets us avoid an allocation
      or separate one-byte entry in our iovec.
    */
    if (packet_num == 0 && include_command) {
      buf[4] = command;
      (*vec).iov_len++;
      /* Our command byte counts against the packet size. */
      ++bytes_queued;
    }

    ++vec;

    /* Second iovec (if any), our optional prefix. */
    if (packet_num == 0 && optional_prefix != nullptr) {
      (*vec).iov_base = const_cast<uchar *>(optional_prefix);
      (*vec).iov_len = prefix_len;
      bytes_queued += prefix_len;
      ++vec;
    }
    /*
      Final iovec, the payload itself. Send however many bytes from
      packet we have left, and advance our packet pointer.
    */
    const size_t remaining_bytes = packet_size - bytes_queued;
    (*vec).iov_base = const_cast<uchar *>(packet);
    (*vec).iov_len = remaining_bytes;

    bytes_queued += remaining_bytes;

    packet += remaining_bytes;
    total_len -= bytes_queued;
    /* clang-format off */
    /*
      If we have a payload to compress, then compress_packet will add
      compression headers for us. This is what we have at this point where
      each line is an iovec.
      | len              |cpn|uncompress len| len                                    | pn | command |
      | prefix + command | 0 |0             | total_len = command + prefix + payload | 0  |COM_*    |

      | prefix |
      |...     |

      |payload |
      |...     |

      We want to transform into this:

      | len             |cpn|uncompress len| len                                    | pn | command |
      | prefix +command | 0 |0             | total_len = command + prefix + payload | 0  |COM_*    |

      | prefix |
      |...     |

      | len                     |cpn|uncompress len| compressed payload |
      | len(compressed payload) |  1|  len(payload)|  compress(payload) |
    */
    /* clang-format on */

    if (net->compress && remaining_bytes) {
      (*vec).iov_base =
          compress_packet(net, (uchar *)(*vec).iov_base, &(*vec).iov_len);
      if (!(*vec).iov_base) {
        reset_packet_write_state(net);
        return 0;
      }
      compressed_buffers[net_async->compressed_buffers_size++] =
          (uchar *)(*vec).iov_base;
    }
    ++vec;

    /* Make sure we sent entire packets. */
    if (total_len > 0) {
      assert(packet_size == MAX_PACKET_LENGTH);
    }
  }

  /* Make sure we don't have anything left to send. */
  assert(total_len == 0);

  net_async->async_write_vector_size = (vec - net_async->async_write_vector);
  net_async->async_write_vector_current = 0;
  /*
   This is needed because the packet reading code in the net_read_packet_header
   uses pkt_nr for verification.
  */
  if (net->compress) net->pkt_nr = net->compress_pkt_nr;
  return 1;
}

static net_async_status net_write_vector_nonblocking(NET *net, ssize_t *res) {
  NET_ASYNC *net_async = NET_ASYNC_DATA(net);
  struct io_vec *vec =
      net_async->async_write_vector + net_async->async_write_vector_current;
  DBUG_TRACE;

  while (net_async->async_write_vector_current !=
         net_async->async_write_vector_size) {
    if (vio_is_blocking(net->vio)) {
      vio_set_blocking_flag(net->vio, false);
    }
    *res = vio_write(net->vio, (uchar *)vec->iov_base, vec->iov_len);

    if (*res < 0) {
      if (socket_errno == SOCKET_EAGAIN ||
          (SOCKET_EAGAIN != SOCKET_EWOULDBLOCK &&
           socket_errno == SOCKET_EWOULDBLOCK)) {
        /*
          In the unlikely event that there is a renegotiation and
          SSL_ERROR_WANT_READ is returned, set blocking state to read.
        */
        if (static_cast<size_t>(*res) == VIO_SOCKET_WANT_READ) {
          net_async->async_blocking_state = NET_NONBLOCKING_READ;
        } else {
          net_async->async_blocking_state = NET_NONBLOCKING_WRITE;
        }
        return NET_ASYNC_NOT_READY;
      }
      return NET_ASYNC_COMPLETE;
    }
    const size_t bytes_written = static_cast<size_t>(*res);
    vec->iov_len -= bytes_written;
    vec->iov_base = (char *)vec->iov_base + bytes_written;

    if (vec->iov_len != 0) break;

    ++net_async->async_write_vector_current;
    vec++;
  }
  if (net_async->async_write_vector_current ==
      net_async->async_write_vector_size) {
    return NET_ASYNC_COMPLETE;
  }

  net_async->async_blocking_state = NET_NONBLOCKING_WRITE;
  return NET_ASYNC_NOT_READY;
}

/**
  Send a command to the server in asynchronous way. This function will first
  populate all headers in NET::async_write_headers, followed by payload in
  NET::async_write_vector. Once header and payload is populated in NET, were
  call net_write_vector_nonblocking to send the packets to server in an
  asynchronous way.
*/
net_async_status net_write_command_nonblocking(NET *net, uchar command,
                                               const uchar *prefix,
                                               size_t prefix_len,
                                               const uchar *packet,
                                               size_t packet_len, bool *res) {
  net_async_status status;
  NET_ASYNC *net_async = NET_ASYNC_DATA(net);
  ssize_t rc;
  DBUG_TRACE;
  DBUG_DUMP("net write prefix", prefix, prefix_len);
  DBUG_DUMP("net write pkt", packet, packet_len);
  if (unlikely(!net->vio)) {
    /* nowhere to write */
    *res = false;
    goto done;
  }

  switch (net_async->async_operation) {
    case NET_ASYNC_OP_IDLE:
      if (!begin_packet_write_state(net, command, packet, packet_len, prefix,
                                    prefix_len)) {
        *res = false;
        goto done;
      }
      net_async->async_operation = NET_ASYNC_OP_WRITING;
      [[fallthrough]];
    case NET_ASYNC_OP_WRITING:
      status = net_write_vector_nonblocking(net, &rc);
      if (status == NET_ASYNC_COMPLETE) {
        if (rc < 0) {
          *res = true;
        } else {
          *res = false;
        }
        goto done;
      }
      return NET_ASYNC_NOT_READY;
    default:
      assert(false);
      *res = true;
      return NET_ASYNC_COMPLETE;
  }

done:
  reset_packet_write_state(net);
  net_async->async_operation = NET_ASYNC_OP_IDLE;
  return NET_ASYNC_COMPLETE;
}

/*
  Non blocking version of my_net_write().
*/
net_async_status my_net_write_nonblocking(NET *net, const uchar *packet,
                                          size_t len, bool *res) {
  return net_write_command_nonblocking(net, COM_END, packet, len, nullptr, 0,
                                       res);
}

/**
  Send a command to the server.

    The reason for having both header and packet is so that libmysql
    can easy add a header to a special command (like prepared statements)
    without having to re-alloc the string.

    As the command is part of the first data packet, we have to do some data
    juggling to put the command in there, without having to create a new
    packet.

    This function will split big packets into sub-packets if needed.
    (Each sub packet can only be 2^24 bytes)

  @param net		NET handler
  @param command	Command in MySQL server (enum enum_server_command)
  @param header	Header to write after command
  @param head_len	Length of header
  @param packet	Query or parameter to query
  @param len		Length of packet

  @retval
    0	ok
  @retval
    1	error
*/

bool net_write_command(NET *net, uchar command, const uchar *header,
                       size_t head_len, const uchar *packet, size_t len) {
  /* turn off non blocking operations */
  if (!vio_is_blocking(net->vio)) vio_set_blocking_flag(net->vio, true);

  size_t length = len + 1 + head_len; /* 1 extra byte for command */
  uchar buff[NET_HEADER_SIZE + 1];
  uint header_size = NET_HEADER_SIZE + 1;
  DBUG_TRACE;
  DBUG_PRINT("enter", ("length: %lu", (ulong)len));

  buff[4] = command; /* For first packet */

  if (length >= MAX_PACKET_LENGTH) {
    /* Take into account that we have the command in the first header */
    len = MAX_PACKET_LENGTH - 1 - head_len;
    do {
      int3store(buff, MAX_PACKET_LENGTH);
      buff[3] = (uchar)net->pkt_nr++;
      if (net_write_buff(net, buff, header_size) ||
          net_write_buff(net, header, head_len) ||
          net_write_buff(net, packet, len)) {
        return true;
      }
      packet += len;
      length -= MAX_PACKET_LENGTH;
      len = MAX_PACKET_LENGTH;
      head_len = 0;
      header_size = NET_HEADER_SIZE;
    } while (length >= MAX_PACKET_LENGTH);
    len = length; /* Data left to be written */
  }
  int3store(buff, static_cast<uint>(length));
  buff[3] = (uchar)net->pkt_nr++;
  const bool rc = net_write_buff(net, buff, header_size) ||
                  (head_len && net_write_buff(net, header, head_len)) ||
                  net_write_buff(net, packet, len) || net_flush(net);
  return rc;
}

/**
  Caching the data in a local buffer before sending it.

   Fill up net->buffer and send it to the client when full.

    If the rest of the to-be-sent-packet is bigger than buffer,
    send it in one big block (to avoid copying to internal buffer).
    If not, copy the rest of the data to the buffer and return without
    sending data.

  @param net		Network handler
  @param packet	Packet to send
  @param len		Length of packet

  @note
    The cached buffer can be sent as it is with 'net_flush()'.
    In this code we have to be careful to not send a packet longer than
    MAX_PACKET_LENGTH to net_write_packet() if we are using the compressed
    protocol as we store the length of the compressed packet in 3 bytes.

  @retval
    0	ok
  @retval
    1   error
*/

static bool net_write_buff(NET *net, const uchar *packet, size_t len) {
  DBUG_TRACE;
  ulong left_length;
  if (net->compress && net->max_packet > MAX_PACKET_LENGTH)
    left_length = (ulong)(MAX_PACKET_LENGTH - (net->write_pos - net->buff));
  else
    left_length = (ulong)(net->buff_end - net->write_pos);

#ifdef DEBUG_DATA_PACKETS
  DBUG_DUMP("data", packet, len);
#endif
  if (len > left_length) {
    if (net->write_pos != net->buff) {
      /* Fill up already used packet and write it */
      memcpy(net->write_pos, packet, left_length);
      if (net_write_packet(net, net->buff,
                           (size_t)(net->write_pos - net->buff) + left_length))
        return true;
      net->write_pos = net->buff;
      packet += left_length;
      len -= left_length;
    }
    if (net->compress) {
      /*
        We can't have bigger packets than 16M with compression
        Because the uncompressed length is stored in 3 bytes
      */
      left_length = MAX_PACKET_LENGTH;
      while (len > left_length) {
        if (net_write_packet(net, packet, left_length)) return true;
        packet += left_length;
        len -= left_length;
      }
    }
    if (len > net->max_packet) return net_write_packet(net, packet, len);
    /* Send out rest of the blocks as full sized blocks */
  }
  if (len > 0) memcpy(net->write_pos, packet, len);
  net->write_pos += len;
  return false;
}

/**
  Write a determined number of bytes to a network handler.

  @param  net     NET handler.
  @param  buf     Buffer containing the data to be written.
  @param  count   The length, in bytes, of the buffer.

  @return true on error, false on success.
*/

static bool net_write_raw_loop(NET *net, const uchar *buf, size_t count) {
  unsigned int retry_count = 0;

  while (count) {
    const size_t sentcnt = vio_write(net->vio, buf, count);

    /* VIO_SOCKET_ERROR (-1) indicates an error. */
    if (sentcnt == VIO_SOCKET_ERROR) {
      /* A recoverable I/O error occurred? */
      if (net_should_retry(net, &retry_count))
        continue;
      else
        break;
    }

    count -= sentcnt;
    buf += sentcnt;
#ifdef MYSQL_SERVER
    thd_increment_bytes_sent(sentcnt);
#endif
  }

  /* On failure, propagate the error code. */
  if (count) {
#ifdef MYSQL_SERVER
    /* Socket should be closed. */
    net->error = NET_ERROR_SOCKET_UNUSABLE;
#else
    /* Socket has failed for writing but it might still work for reading. */
    net->error = NET_ERROR_SOCKET_NOT_WRITABLE;
#endif
    /* Interrupted by a timeout? */
    if (vio_was_timeout(net->vio))
      net->last_errno = ER_NET_WRITE_INTERRUPTED;
    else
      net->last_errno = ER_NET_ERROR_ON_WRITE;

#ifdef MYSQL_SERVER
    my_error(net->last_errno, MYF(0));
#endif
  }

  return count != 0;
}

/* clang-format off */
/**
  @page page_protocol_basic_compression Compression

  Compression is:
    - its own protocol layer
    - transparent to the other MySQL protocol layers
    - compressing a string of bytes (which may even be a part of
      @ref sect_protocol_basic_packets_packet)

  It is enabled if:
    - the server announces ::CLIENT_COMPRESS or
      ::CLIENT_ZSTD_COMPRESSION_ALGORITHM in its
      @ref page_protocol_connection_phase_packets_protocol_handshake based on
      variable protocol_compression_algorithms and
    - the client does following:
      - if client flags match with server flags, then client announces the
        matching flag as part of @ref page_protocol_connection_phase_packets_protocol_handshake_response
        if matching flag is ::CLIENT_ZSTD_COMPRESSION_ALGORITHM then client sends
        extra 1 byte in @ref page_protocol_connection_phase_packets_protocol_handshake_response
      - if client flags do not match then connection fallsback to uncompressed mode.
    - Server finishes the @ref page_protocol_connection_phase with an
      @ref page_protocol_basic_ok_packet.

   If both ::CLIENT_COMPRESS and ::CLIENT_ZSTD_COMPRESSION_ALGORITHM are set
   then zlib is used.

   @subpage page_protocol_basic_compression_packet
*/

/**
  @page page_protocol_basic_compression_packet Compressed Packet

  The compressed packet consists of a @ref sect_protocol_basic_compression_packet_header
  and a payload which is either a @ref sect_protocol_basic_compression_packet_compressed_payload
  or @ref sect_protocol_basic_compression_packet_uncompressed_payload.

  @sa ::compress_packet, ::CLIENT_COMPRESS

  @section sect_protocol_basic_compression_packet_header Compressed Packet Header

  <table>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int3 "int&lt;3&gt;"</td>
  <td>length of compressed payload</td>
  <td>raw packet length minus the size of the compressed packet header
     (7 bytes) itself.</td></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
  <td>compressed sequence id</td>
  <td>Sequence ID of the compressed packets, reset in the same way as the
     @ref sect_protocol_basic_packets_packet, but incremented independently</td></tr>
  <tr><td>@ref a_protocol_type_int3 "int&lt;3&gt;"</td>
  <td>length of uncompressed payload</td>
  <td>Length of payload before compression</td></tr>
  </table>

  @section sect_protocol_basic_compression_packet_compressed_payload Compressed Payload

  If the length of *length of payload before compression* is more than 0 the
  @ref sect_protocol_basic_compression_packet_header is followed by the
  compressed payload.

  Depending on which capability flags are set it uses the *deflate* algorithm as described in
  [RFC 1951](http://tools.ietf.org/html/rfc1951.html) and implemented in
  [zlib](http://zlib.org/) or [Zstandard](https://facebook.github.io/zstd/).

  When using zlib, the header of the compressed packet has the parameters of the `uncompress()` function in mind:

  ~~~~~~~~~~~~~
  ZEXTERN int ZEXPORT uncompress OF((Bytef *dest,   uLongf *destLen,
                                 const Bytef *source, uLong sourceLen));
  ~~~~~~~~~~~~~

  The payload can be anything from a piece of a MySQL Packet to several
  MySQL Packets. The client or server may bundle several MySQL packets,
  compress it and send it as one compressed packet.

  @subsection sect_protocol_basic_compression_packet_compressed_payload_single Example: One MySQL Packet

  A ::COM_QUERY for `select "012345678901234567890123456789012345"` without
  ::CLIENT_COMPRESS has a *payload length* of 46 bytes and looks like:

  ~~~~~~~~~~~~~
  2e 00 00 00 03 73 65 6c    65 63 74 20 22 30 31 32    .....select "012
  33 34 35 36 37 38 39 30    31 32 33 34 35 36 37 38    3456789012345678
  39 30 31 32 33 34 35 36    37 38 39 30 31 32 33 34    9012345678901234
  35 22                                                 5"
  ~~~~~~~~~~~~~

  With ::CLIENT_COMPRESS the packet is:

  ~~~~~~~~~~~~~
  22 00 00 00 32 00 00 78    9c d3 63 60 60 60 2e 4e    "...2..x..c```.N
  cd 49 4d 2e 51 50 32 30    34 32 36 31 35 33 b7 b0    .IM.QP20426153..
  c4 cd 52 02 00 0c d1 0a    6c                         ..R.....l
  ~~~~~~~~~~~~~

  <table>
  <tr><th>comp-length</th><th>seq-id</th><th>uncomp-len</th><th>Compressed Payload</th></tr>
  <tr><td>`22 00 00`</td><td>`00`</td><td>`32 00 00`</td><td>compress("\x2e\x00\x00\x00\x03select ...")`</td></td>
  </table>

  The compressed packet is 41 bytes long and splits into:

  ~~~~~~~~~~~~~~
  raw packet length                      -> 41
  compressed payload length   = 22 00 00 -> 34 (41 - 7)
  sequence id                 = 00       ->  0
  uncompressed payload length = 32 00 00 -> 50
  ~~~~~~~~~~~~~~

  @subsection sect_protocol_basic_compression_packet_compressed_payload_multi Example: Several MySQL Packets

  Executing `SELECT repeat("a", 50)` results in uncompressed  ProtocolText::Resultset like:
  ~~~~~~~~~~~~~
  01 00 00 01 01 25 00 00    02 03 64 65 66 00 00 00    .....%....def...
  0f 72 65 70 65 61 74 28    22 61 22 2c 20 35 30 29    .repeat("a", 50)
  00 0c 08 00 32 00 00 00    fd 01 00 1f 00 00 05 00    ....2...........
  00 03 fe 00 00 02 00 33    00 00 04 32 61 61 61 61    .......3...2aaaa
  61 61 61 61 61 61 61 61    61 61 61 61 61 61 61 61    aaaaaaaaaaaaaaaa
  61 61 61 61 61 61 61 61    61 61 61 61 61 61 61 61    aaaaaaaaaaaaaaaa
  61 61 61 61 61 61 61 61    61 61 61 61 61 61 05 00    aaaaaaaaaaaaaa..
  00 05 fe 00 00 02 00                                  .......
  ~~~~~~~~~~~~~

  which consists of 5 @ref sect_protocol_basic_packets_packet :

  - `01 00 00 01 01`
  - `25 00 00 02 03 64 65 66 00 00 00 0f 72 65 70 65 61 74 28 22 61 22 2c 20 35 30 29 00 0c 08 00 32 00 00 00 fd 01 00 1f 00 00`
  - `05 00 00 03 fe 00 00 02 00`
  - `33 00 00 04 32 61 61 61 61 ...`
  - `05 00 00 05 fe 00 00 02 00`

  If compression is enabled a compressed packet containing the compressed
  version of all 5 packets is sent to the client:

  ~~~~~~~~~~~~~
  4a 00 00 01 77 00 00 78    9c 63 64 60 60 64 54 65    J...w..x.cd``dTe
  60 60 62 4e 49 4d 63 60    60 e0 2f 4a 2d 48 4d 2c    ``bNIMc``./J-HM,
  d1 50 4a 54 d2 51 30 35    d0 64 e0 e1 60 30 02 8a    .PJT.Q05.d..`0..
  ff 65 64 90 67 60 60 65    60 60 fe 07 54 cc 60 cc    .ed.g``e``..T.`.
  c0 c0 62 94 48 32 00 ea    67 05 eb 07 00 8d f9 1c    ..b.H2..g.......
  64                                                    d
  ~~~~~~~~~~~~~

  @note sending a MySQL Packet of the size 2<sup>24</sup>-5 to 2<sup>24</sup>-1
  via compression leads to at least one extra compressed packet.
  If the uncompressed MySQL Packet is like
  ~~~~~~~~~~~~~~
  fe ff ff 03 ... -- length = 2^24-2, sequence id = 3
  ~~~~~~~~~~~~~~
  compressing it would result in the *length of payload before compression*
  in the @ref sect_protocol_basic_compression_packet_header being:
  ~~~~~~~~~~~~~~
  length of mysql packet payload:       2^24-2
  length of mysql packet header:        4
  length of payload before compression: 2^24+2
  ~~~~~~~~~~~~~~
  which can not be represented in one compressed packet.
  Instead two or more packets have to be sent.

  @section sect_protocol_basic_compression_packet_uncompressed_payload Uncompressed Payload

  For small packets it may be to costly to compress the packet:
  - compressing the packet may lead to more data and sending the
    data uncompressed
  - CPU overhead may be not worth to compress the data
    @par Tip
    Usually payloads less than 50 bytes (::MIN_COMPRESS_LENGTH) aren't compressed.


  To send an @ref sect_protocol_basic_compression_packet_uncompressed_payload :
  - set *length of payload before compression* in
    @ref sect_protocol_basic_compression_packet_header to 0
  - The @ref sect_protocol_basic_compression_packet_compressed_payload contains
    the @ref sect_protocol_basic_compression_packet_uncompressed_payload instead.

  Sending a `SELECT 1` query as @ref sect_protocol_basic_compression_packet_uncompressed_payload
  to the server looks like:
  ~~~~~~~~~~~~~~
  0d 00 00 00 00 00 00 09    00 00 00 03 53 45 4c 45    ............SELE
  43 54 20 31                                           CT 1
  ~~~~~~~~~~~~~~

  decodes into:
  ~~~~~~~~~~~~~~
  raw packet length                      -> 20
  compressed payload length   = 0d 00 00 -> 13 (20 - 7)
  sequence id                 = 00       ->  0
  uncompressed payload length = 00 00 00 -> uncompressed
  ~~~~~~~~~~~~~~

  ... with the *uncompressed payload* starting right after the 7 byte header:

  ~~~~~~~~~~~~~~
  09 00 00 00 03 53 45 4c 45 43 54 20 31 -- SELECT 1
  ~~~~~~~~~~~~~~
*/
/* clang-format on */

/**
  Compress and encapsulate a packet into a compressed packet.

  @param          net      NET handler.
  @param          packet   The packet to compress.
  @param[in,out]  length   Length of the packet.

  See @ref sect_protocol_basic_compression_packet_header for a
  description of the header structure.

  @return Pointer to the (new) compressed packet.
*/

static uchar *compress_packet(NET *net, const uchar *packet, size_t *length) {
  uchar *compr_packet;
  size_t compr_length = 0;
  const uint header_length = NET_HEADER_SIZE + COMP_HEADER_SIZE;

  compr_packet = (uchar *)my_malloc(key_memory_NET_compress_packet,
                                    *length + header_length, MYF(MY_WME));

  if (compr_packet == nullptr) return nullptr;

  memcpy(compr_packet + header_length, packet, *length);

  mysql_compress_context *compress_ctx = compress_context(net);

  /* Compress the encapsulated packet. */
  if (my_compress(compress_ctx, compr_packet + header_length, length,
                  &compr_length)) {
    /*
      If the length of the compressed packet is larger than the
      original packet, the original packet is sent uncompressed.
    */
    compr_length = 0;
  }

  /* Length of the compressed (original) packet. */
  int3store(&compr_packet[NET_HEADER_SIZE], static_cast<uint>(compr_length));
  /* Length of this packet. */
  int3store(compr_packet, static_cast<uint>(*length));
  /* Packet number. */
  compr_packet[3] = (uchar)(net->compress_pkt_nr++);

  *length += header_length;

  return compr_packet;
}

/**
  Write a MySQL protocol packet to the network handler.

  @param  net     NET handler.
  @param  packet  The packet to write.
  @param  length  Length of the packet.

  @remark The packet might be encapsulated into a compressed packet.

  @return true on error, false on success.
*/

bool net_write_packet(NET *net, const uchar *packet, size_t length) {
  bool res;
  DBUG_TRACE;

  /* Socket can't be used */
  if (net->error == NET_ERROR_SOCKET_UNUSABLE ||
      net->error == NET_ERROR_SOCKET_NOT_WRITABLE)
    return true;

  net->reading_or_writing = 2;
#ifdef MYSQL_SERVER
  server_store_cached_values(net);
#endif

  const bool do_compress = net->compress;
  if (do_compress) {
    if ((packet = compress_packet(net, packet, &length)) == nullptr) {
      net->error = NET_ERROR_SOCKET_UNUSABLE;
      net->last_errno = ER_OUT_OF_RESOURCES;
      /* In the server, allocation failure raises a error. */
      net->reading_or_writing = 0;
#ifdef MYSQL_SERVER
      server_store_cached_values(net);
#endif
      return true;
    }
  }

#ifdef DEBUG_DATA_PACKETS
  DBUG_DUMP("data", packet, length);
#endif

  res = net_write_raw_loop(net, packet, length);

  if (do_compress) my_free(const_cast<uchar *>(packet));

  net->reading_or_writing = 0;
#ifdef MYSQL_SERVER
  server_store_cached_values(net);
#endif

  /* Socket can't be used any more */
  if (net->error == NET_ERROR_SOCKET_NOT_READABLE) {
    net->error = NET_ERROR_SOCKET_UNUSABLE;
    return true;
  }

  return res;
}

/*****************************************************************************
** Read something from server/clinet
*****************************************************************************/

/**
  Read a determined number of bytes from a network handler.

  @param  net     NET handler.
  @param  count   The number of bytes to read.

  @return true on error, false on success.
*/

static bool net_read_raw_loop(NET *net, size_t count) {
  DBUG_TRACE;
  bool eof = false;
  unsigned int retry_count = 0;
  uchar *buf = net->buff + net->where_b;
  bool timeout_on_full_packet = false;
  bool is_packet_timeout = false;
#ifdef MYSQL_SERVER
  NET_SERVER *server_ext = static_cast<NET_SERVER *>(net->extension);
  if (server_ext) timeout_on_full_packet = server_ext->timeout_on_full_packet;
#endif

  time_t start_time = 0;
  if (timeout_on_full_packet) start_time = time(&start_time);
  while (count) {
    const size_t recvcnt = vio_read(net->vio, buf, count);

    /* VIO_SOCKET_ERROR (-1) indicates an error. */
    if (recvcnt == VIO_SOCKET_ERROR) {
      /* A recoverable I/O error occurred? */
      if (net_should_retry(net, &retry_count))
        continue;
      else
        break;
    }
    /* Zero indicates end of file. */
    else if (!recvcnt) {
      eof = true;
      break;
    }

    count -= recvcnt;
    buf += recvcnt;
#ifdef MYSQL_SERVER
    thd_increment_bytes_received(recvcnt);
#endif
    if (timeout_on_full_packet) {
      time_t current_time = time(&current_time);
      if (static_cast<unsigned int>(current_time - start_time) >
          net->read_timeout) {
        is_packet_timeout = true;
        break;
      }
    }
  }

  /* On failure, propagate the error code. */
  if (count) {
    /* Interrupted by a timeout? */
    if (!eof && (vio_was_timeout(net->vio) || is_packet_timeout))
      net->last_errno = ER_NET_READ_INTERRUPTED;
    else
      net->last_errno = ER_NET_READ_ERROR;

#ifdef MYSQL_SERVER
    /* First packet always wait for net_wait_timeout */
    if (net->pkt_nr == 0 && (vio_was_timeout(net->vio) || is_packet_timeout)) {
      net->last_errno = ER_CLIENT_INTERACTION_TIMEOUT;

      /* Socket should be closed after trying to write/send error. */
      THD *thd = current_thd;
      if (thd) {
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::microseconds(my_micro_time()));
        auto start = std::chrono::seconds(thd->start_time.tv_sec);

        auto dur = now - start;
        auto wtout = std::chrono::seconds(thd_get_net_wait_timeout(thd));

        Security_context *sctx = thd->security_context();
        Auth_id auth_id(sctx->priv_user(), sctx->priv_host());

        LogErr(INFORMATION_LEVEL, ER_LOG_CLIENT_INTERACTION_TIMEOUT,
               auth_id.auth_str().c_str(), vio_was_timeout(net->vio),
               is_packet_timeout, dur.count(), wtout.count());
        if (dur < wtout) {
          LogErr(ERROR_LEVEL, ER_CONDITIONAL_DEBUG,
                 "IO-layer timeout before wait_timeout was reached.");
        }
      } else {
        LogErr(INFORMATION_LEVEL, ER_NET_WAIT_ERROR);
      }
    }
    net->error = NET_ERROR_SOCKET_NOT_READABLE;
    /*
      Attempt to send error message to client although the client won't be
      expecting messages. If later the client tries to send a command and fail
      it will instead check if it can read an error message.
    */
    my_error(net->last_errno, MYF(0));
#else
    /* Socket should be closed. */
    net->error = NET_ERROR_SOCKET_UNUSABLE;
#endif
  }

  return count != 0;
}

/**
  Read the header of a packet. The MySQL protocol packet header
  consists of the length, in bytes, of the payload (packet data)
  and a serial number.

  @remark The encoded length is the length of the packet payload,
          which does not include the packet header.

  @remark The serial number is used to ensure that the packets are
          received in order. If the packet serial number does not
          match the expected value, a error is returned.

  @param  net  NET handler.

  @return true on error, false on success.
*/

static bool net_read_packet_header(NET *net) {
  uchar pkt_nr;
  size_t count = NET_HEADER_SIZE;
  bool rc;

  if (net->compress) count += COMP_HEADER_SIZE;

#ifdef MYSQL_SERVER
  NET_SERVER *server_extension;

  server_extension = static_cast<NET_SERVER *>(net->extension);

  if (server_extension != nullptr && server_extension->m_user_data != nullptr) {
    void *user_data = server_extension->m_user_data;
    assert(server_extension->m_before_header != nullptr);
    assert(server_extension->m_after_header != nullptr);

    server_extension->m_before_header(net, user_data, count);
    rc = net_read_raw_loop(net, count);
    server_extension->m_after_header(net, user_data, count, rc);
  } else
#endif
  {
    rc = net_read_raw_loop(net, count);
  }

  if (rc) return true;

  DBUG_DUMP("packet_header", net->buff + net->where_b, NET_HEADER_SIZE);

  pkt_nr = net->buff[net->where_b + 3];

  /*
    Verify packet serial number against the truncated packet counter.
    The local packet counter must be truncated since its not reset.
  */
  if (pkt_nr != (uchar)net->pkt_nr) {
    /* Not a NET error on the client. XXX: why? */
#if !defined(MYSQL_SERVER)
    DBUG_PRINT("info", ("pkt_nr %u net->pkt_nr %u", pkt_nr, net->pkt_nr));
    if (net->pkt_nr == 1) {
      assert(net->where_b == 0);
      /*
        Server may have sent an error before it received our new command.
        Perhaps due to wait_timeout.
        Only use what is already read and then close the socket.
      */
      net->error = NET_ERROR_SOCKET_UNUSABLE;
      net->last_errno = ER_NET_PACKETS_OUT_OF_ORDER;
      net->pkt_nr = pkt_nr + 1;

      /*
        The caller should handle the error code in the packet
        and the socket are blocked from further usage,
        so reading the packet header was OK.
      */
      return false;
    }
#endif
#if defined(MYSQL_SERVER)
    my_error(ER_NET_PACKETS_OUT_OF_ORDER, MYF(0));
#elif defined(EXTRA_DEBUG)
    /*
      We don't make noise server side, since the client is expected
      to break the protocol for e.g. --send LOAD DATA .. LOCAL where
      the server expects the client to send a file, but the client
      may reply with a new command instead.
    */
    my_message_local(ERROR_LEVEL, EE_PACKETS_OUT_OF_ORDER, (uint)pkt_nr,
                     net->pkt_nr);
    assert(pkt_nr == net->pkt_nr);
#endif
    return true;
  }

  net->pkt_nr++;

  return false;
}

/*
  Helper function to read up to count bytes from the network connection/

   Returns packet_error (-1) on EOF or other errors, 0 if the read
   would block, and otherwise the number of bytes read (which may be
   less than the requested amount).

   When 0 is returned the async_blocking_state is set inside this function.
   With SSL, the async blocking state can also become NET_NONBLOCKING_WRITE
   (when renegotiation occurs).
*/
static ulong net_read_available(NET *net, size_t count) {
  size_t recvcnt;
  DBUG_TRACE;
  NET_ASYNC *net_async = NET_ASYNC_DATA(net);
  if (net_async->cur_pos + count > net->buff + net->max_packet) {
    if (net_realloc(net, net->max_packet + count)) {
      return packet_error;
    }
  }
  if (vio_is_blocking(net->vio)) {
    vio_set_blocking_flag(net->vio, false);
  }
  recvcnt = vio_read(net->vio, net_async->cur_pos, count);
  /*
    When OpenSSL is used in non-blocking mode, it is possible that an
    SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE error is returned after a
    SSL_read() operation (if a renegotiation takes place).
    We are treating this case here and signaling correctly the next expected
    operation in the async_blocking_state.
  */
  if (recvcnt == VIO_SOCKET_WANT_READ) {
    net_async->async_blocking_state = NET_NONBLOCKING_READ;
    return 0;
  } else if (recvcnt == VIO_SOCKET_WANT_WRITE) {
    net_async->async_blocking_state = NET_NONBLOCKING_WRITE;
    return 0;
  }

  /* Call would block, just return with socket_errno set */
  if ((recvcnt == VIO_SOCKET_ERROR) &&
      (socket_errno == SOCKET_EAGAIN || (SOCKET_EAGAIN != SOCKET_EWOULDBLOCK &&
                                         socket_errno == SOCKET_EWOULDBLOCK))) {
    net_async->async_blocking_state = NET_NONBLOCKING_READ;
    return 0;
  }

  /* Not EOF and not an error?  Return the bytes read.*/
  if (recvcnt != 0 && recvcnt != VIO_SOCKET_ERROR) {
    net_async->cur_pos += recvcnt;
#ifdef MYSQL_SERVER
    thd_increment_bytes_received(recvcnt);
#endif
    return recvcnt;
  }

  /* EOF or hard failure; socket should be closed. */
  net->error = NET_ERROR_SOCKET_UNUSABLE;
  net->last_errno = ER_NET_READ_ERROR;
  return packet_error;
}

/* Read actual data from the packet */
static net_async_status net_read_data_nonblocking(NET *net, size_t count,
                                                  bool *err_ptr) {
  DBUG_TRACE;
  NET_ASYNC *net_async = NET_ASYNC_DATA(net);
  size_t bytes_read = 0;
  ulong rc;
  switch (net_async->async_operation) {
    case NET_ASYNC_OP_IDLE:
      net_async->async_bytes_wanted = count;
      net_async->async_operation = NET_ASYNC_OP_READING;
      net_async->cur_pos = net->buff + net->where_b;
      [[fallthrough]];
    case NET_ASYNC_OP_READING:
      rc = net_read_available(net, net_async->async_bytes_wanted);
      if (rc == packet_error) {
        *err_ptr = true;
        net_async->async_operation = NET_ASYNC_OP_IDLE;
        return NET_ASYNC_COMPLETE;
      }
      bytes_read = (size_t)rc;
      net_async->async_bytes_wanted -= bytes_read;
      if (net_async->async_bytes_wanted != 0) {
        DBUG_PRINT("partial read",
                   ("wanted/remaining/read: %zu, %zu, %zu", count,
                    net_async->async_bytes_wanted, bytes_read));
        return NET_ASYNC_NOT_READY;
      }
      net_async->async_operation = NET_ASYNC_OP_COMPLETE;
      [[fallthrough]];
    case NET_ASYNC_OP_COMPLETE:
      net_async->async_bytes_wanted = 0;
      net_async->async_operation = NET_ASYNC_OP_IDLE;
      *err_ptr = false;
      DBUG_PRINT("read complete", ("read: %zu", count));
      return NET_ASYNC_COMPLETE;
    default:
      /* error, sure wish we could log something here */
      assert(false);
      net_async->async_bytes_wanted = 0;
      net_async->async_operation = NET_ASYNC_OP_IDLE;
      *err_ptr = true;
      return NET_ASYNC_COMPLETE;
  }
}

static net_async_status net_read_packet_header_nonblocking(NET *net,
                                                           bool *err_ptr) {
  DBUG_TRACE;
  uchar pkt_nr;
  size_t bytes_wanted = NET_HEADER_SIZE;
  if (net->compress) bytes_wanted += COMP_HEADER_SIZE;
  if (net_read_data_nonblocking(net, bytes_wanted, err_ptr) ==
      NET_ASYNC_NOT_READY) {
    return NET_ASYNC_NOT_READY;
  }
  if (*err_ptr) {
    return NET_ASYNC_COMPLETE;
  }

  DBUG_DUMP("packet_header", net->buff + net->where_b, bytes_wanted);

  pkt_nr = net->buff[net->where_b + 3];

  /*
    Verify packet serial number against the truncated packet counter.
    The local packet counter must be truncated since its not reset.
  */
  if (pkt_nr != (uchar)net->pkt_nr) {
    /* Not a NET error on the client. XXX: why? */
#if defined(MYSQL_SERVER)
    my_error(ER_NET_PACKETS_OUT_OF_ORDER, MYF(0));
#elif defined(EXTRA_DEBUG)
    /*
      We don't make noise server side, since the client is expected
      to break the protocol for e.g. --send LOAD DATA .. LOCAL where
      the server expects the client to send a file, but the client
      may reply with a new command instead.
    */
    fprintf(stderr, "Error: packets out of order (found %u, expected %u)\n",
            (uint)pkt_nr, net->pkt_nr);
    assert(pkt_nr == net->pkt_nr);
#endif
    *err_ptr = true;
    return NET_ASYNC_COMPLETE;
  }

  net->pkt_nr++;

  *err_ptr = false;
  return NET_ASYNC_COMPLETE;
}

/*
  Read packet header followed by packet data in an asynchronous way.
*/
static net_async_status net_read_packet_nonblocking(NET *net, ulong *ret) {
  DBUG_TRACE;
  NET_ASYNC *net_async = NET_ASYNC_DATA(net);
  size_t pkt_data_len;
  size_t async_packet_uncompressed_length = 0;
  bool err;

  switch (net_async->async_packet_read_state) {
    case NET_ASYNC_PACKET_READ_IDLE:
      net_async->async_packet_read_state = NET_ASYNC_PACKET_READ_HEADER;
      net->reading_or_writing = 0;
#ifdef MYSQL_SERVER
      server_store_cached_values(net);
#endif
      [[fallthrough]];
    case NET_ASYNC_PACKET_READ_HEADER:
      /*
        We should reset compress_packet_nr even before reading the header
        because reading can fail and then the compressed packet number won't get
        reset.
      */
      net->compress_pkt_nr = net->pkt_nr;
      if (net_read_packet_header_nonblocking(net, &err) ==
          NET_ASYNC_NOT_READY) {
        return NET_ASYNC_NOT_READY;
      }
      /* Retrieve packet length and number. */
      if (err) goto error;

      net->compress_pkt_nr = net->pkt_nr;

      /* The length of the packet that follows. */
      net_async->async_packet_length = uint3korr(net->buff + net->where_b);
      DBUG_PRINT("info",
                 ("async packet len: %zu", net_async->async_packet_length));

      /*
        If this is using the compressed protocol, the next 3 bytes contain
        the length of the uncompressed contents of the payload.
      */
      if (net->compress) {
        /*
          The following uint3korr() may read 4 bytes, so make sure we don't
          read unallocated or uninitialized memory. The right-hand expression
          must match the size of the buffer allocated in net_realloc().
        */
        assert(net->where_b + NET_HEADER_SIZE + sizeof(uint32) <=
               net->max_packet + NET_HEADER_SIZE + COMP_HEADER_SIZE + 1);
        async_packet_uncompressed_length =
            uint3korr(net->buff + net->where_b + NET_HEADER_SIZE);
      }

      /* End of big multi-packet. */
      if (!net_async->async_packet_length) goto end;

      pkt_data_len = max(net_async->async_packet_length,
                         async_packet_uncompressed_length) +
                     net->where_b;

      /* Expand packet buffer if necessary. */
      if ((pkt_data_len >= net->max_packet) && net_realloc(net, pkt_data_len))
        goto error;

      net_async->async_packet_read_state = NET_ASYNC_PACKET_READ_BODY;
      [[fallthrough]];
    case NET_ASYNC_PACKET_READ_BODY:
      if (net_read_data_nonblocking(net, net_async->async_packet_length,
                                    &err) == NET_ASYNC_NOT_READY) {
        return NET_ASYNC_NOT_READY;
      }

      if (err) goto error;

      net_async->async_packet_read_state = NET_ASYNC_PACKET_READ_COMPLETE;
      [[fallthrough]];

    case NET_ASYNC_PACKET_READ_COMPLETE:
      net_async->async_packet_read_state = NET_ASYNC_PACKET_READ_IDLE;
      break;
  }

end:
  *ret = net_async->async_packet_length;
  net->read_pos = net->buff + net->where_b;
#ifdef DEBUG_DATA_PACKETS
  DBUG_DUMP("async read output", net->read_pos, *ret);
#endif

  net->read_pos[*ret] = 0;
  net->reading_or_writing = 0;
#ifdef MYSQL_SERVER
  server_store_cached_values(net);
#endif
  if (net->compress) {
    mysql_compress_context *mysql_compress_ctx = compress_context(net);
    if (my_uncompress(mysql_compress_ctx, net->buff + net->where_b,
                      net_async->async_packet_length,
                      &(async_packet_uncompressed_length))) {
#ifdef MYSQL_SERVER
      my_error(ER_NET_UNCOMPRESS_ERROR, MYF(0));
#else
      net->error = NET_ERROR_SOCKET_UNUSABLE;  // caller will close socket
      net->last_errno = ER_NET_UNCOMPRESS_ERROR;
#endif
      goto error;
    }
    *ret = async_packet_uncompressed_length;
  }
  return NET_ASYNC_COMPLETE;

error:
  *ret = packet_error;
  net->reading_or_writing = 0;
#ifdef MYSQL_SERVER
  server_store_cached_values(net);
#endif
  return NET_ASYNC_COMPLETE;
}

/**
  Initialize the offsets. If it is first packet then initialize the offsets
  from the beginning else initialize the offsets after the data last read
  in the old packet.

  @param [in, out]  net              NET structure.
  @param [in, out]  start_of_packet  Starting offset of the packet to be read.
                                     In multipackets, points to the next header
                                     that will be erased.
  @param [in, out]  first_packet_offset Starting offset of the packet to read.
  @param [in, out]  multi_byte_packet  Flag that indicate if packet is multibyte
  @param [in, out]  buf_length       End of readable byte.
*/
static void net_read_init_offsets(NET *net, size_t &start_of_packet,
                                  size_t &first_packet_offset,
                                  uint &multi_byte_packet, size_t &buf_length) {
  if (net->remain_in_buf) {
    buf_length = net->buf_length; /* Data left in old packet */
    first_packet_offset = start_of_packet =
        static_cast<size_t>(net->buf_length - net->remain_in_buf);
    /* Restore the character that was overwritten by the end 0 */
    net->buff[start_of_packet] = net->save_char;
  } else {
    /* reuse buffer, as there is nothing in it that we need */
    buf_length = start_of_packet = first_packet_offset = 0;
  }
  multi_byte_packet = 0;
}

/**
  Read the payload length in the packet, if it is found to multipacket omit
  the packet header of the subsequent packets.

  @param [in, out]  net              NET structures
  @param [in, out]  start_of_packet  In multipackets, points to the next header
                                     that will be erased
  @param [in, out]  buf_length       End of readable byte
  @param [in, out]  multi_byte_packet Flag that indicate if packet is multibyte
  @param [in, out]  first_packet_offset Starting offset of the packet to read

  @retval true   The last packet read
  @retval false  Otherwise
*/
static bool net_read_process_buffer(NET *net, size_t &start_of_packet,
                                    size_t &buf_length, uint &multi_byte_packet,
                                    size_t &first_packet_offset) {
  DBUG_TRACE;
begin:
  DBUG_PRINT("info", ("async_buf_length : %zu, remaining_buf : %lu, "
                      "first_packet_offset : %zu,  start_of_packet : %zu, "
                      "net->where_b : %lu, multi_byte_packet : %u ",
                      buf_length, net->remain_in_buf, first_packet_offset,
                      start_of_packet, net->where_b, multi_byte_packet));
  const size_t remain_in_buf = buf_length - start_of_packet;
  if (remain_in_buf >= NET_HEADER_SIZE) {
    const size_t read_length = uint3korr(net->buff + start_of_packet);
    DBUG_PRINT("info", ("read_length : %zu", read_length));
    if (!read_length) {
      start_of_packet += NET_HEADER_SIZE; /* End of multi-byte packet */
      return true;
    }
    if (read_length + NET_HEADER_SIZE <= remain_in_buf) {
      if (multi_byte_packet) {
        /*
          It's never the buffer on the first loop iteration that will have
          multi_byte_packet on. Thus there shall never be a non-zero
          first_packet_offset here.
        */
        assert(first_packet_offset == 0);
        /* Remove packet header for second packet */
        memmove(net->buff + start_of_packet,
                net->buff + start_of_packet + NET_HEADER_SIZE,
                remain_in_buf - NET_HEADER_SIZE);
        start_of_packet += read_length;
        buf_length -= NET_HEADER_SIZE;
      } else {
        start_of_packet += read_length + NET_HEADER_SIZE;
      }
      /* last packet */
      if (read_length < MAX_PACKET_LENGTH) {
        multi_byte_packet = 0; /* No last zero len packet */
        return true;
      }
      multi_byte_packet = NET_HEADER_SIZE;
      /* Move data down to read next data packet after current one */
      if (first_packet_offset) {
        memmove(net->buff, net->buff + first_packet_offset,
                buf_length - first_packet_offset);
        buf_length -= first_packet_offset;
        start_of_packet -= first_packet_offset;
        first_packet_offset = 0;
      }
      goto begin; /* Process the next data packet*/
    }
  }

  /*
    If we reach here, we need to read off network.
    Start by clearing the front of the buffer.
  */
  if (first_packet_offset) {
    memmove(net->buff, net->buff + first_packet_offset,
            buf_length - first_packet_offset);
    buf_length -= first_packet_offset;
    start_of_packet -= first_packet_offset;
    first_packet_offset = 0;
  }
  net->where_b = buf_length;
  return false;
}

/**
  Update the offsets in the NET structure after reading the current packet.

  @param [in, out]  net         NET structure.
  @param [in]  start_of_packet  In multipackets, points to the next header
                                that will be erased.
  @param [in]  first_packet_offset Starting offset of the packet to read.
  @param [in]  buf_length       End of readable byte.
  @param [in]  multi_byte_packet Flag that indicate if packet is multibyte
*/
static ulong net_read_update_offsets(NET *net, size_t start_of_packet,
                                     size_t first_packet_offset,
                                     size_t buf_length,
                                     uint multi_byte_packet) {
  DBUG_TRACE;
  DBUG_PRINT("info", ("multi_byte_packet: %u, first_packet_offset : %zu, "
                      "start_of_packet : %zu",
                      multi_byte_packet, first_packet_offset, start_of_packet));
  net->read_pos = net->buff + first_packet_offset + NET_HEADER_SIZE;
  net->buf_length = buf_length;
  net->remain_in_buf = (ulong)(buf_length - start_of_packet);
  const ulong len = ((ulong)(start_of_packet - first_packet_offset) -
                     NET_HEADER_SIZE - multi_byte_packet);
  if (net->remain_in_buf) {
    /*
      If multi byte packet is non-zero then there is a zero length
      packet at read_pos[len]. Adding the size of one header
      reads the correct byte that will later be replaced. Guarded
      to avoid buffer overflow. If remain_buf = 0 then the char
      won't be restored anyway.
    */
    net->save_char = net->read_pos[len + multi_byte_packet];
  }
  net->read_pos[len] = '\0';  // Safeguard for mysql_use_result.
  DBUG_PRINT("info", ("len :%lu, net->remain_in_buf : %lu, net->read_pos: %d",
                      len, net->remain_in_buf, *(net->read_pos)));
  return len;
}

/*
  If packet is compressed, uncompress it and parse the next available packet
  in the uncompressed packet.

  NET_FIELDS
  ----------
  net->buff           the head of the buffer
  net->buf_length     buff + buff_length is the buffer that contains data
  net->remain_in_buf  the data in [remain_in_buf, buf_length]
                      is data buffered to be read
  net->whereb         Processed position in the buffer
  first_packet_offset Points to the header of the packet to be returned
  start_of_packet     In multipackets, points to the next header that will
                      be erased.
  Example that shows the position of NET_fields to return Packet 2
  -------------------------------------------------------
  |H1|P1|H2|P2|H3|P3|...
  -------------------------------------------------------
   ^     ^  ^        ^
   ^     ^  ^        net->where_b (used for network writes)
   ^     ^  ^        net->buf_length (end of readable bytes)
   ^     ^  ^
   ^     ^  net->read_pos (Return stripped packet)
   ^     ^
   ^     first_packet_offset
   ^
   net->buff
*/
static net_async_status net_read_compressed_nonblocking(NET *net,
                                                        ulong *len_ptr) {
  DBUG_TRACE;
  NET_ASYNC *net_async = NET_ASYNC_DATA(net);
  assert(net->compress);
  ulong &len = *len_ptr;
  if (net_async->mp_state.mp_status != NET_ASYNC_NOT_READY)
    net_read_init_offsets(net, net_async->mp_state.mp_start_of_packet,
                          net_async->mp_state.mp_first_packet_offset,
                          net_async->mp_state.mp_multi_byte_packet,
                          net_async->mp_state.mp_buf_length);

  for (;;) {
    /*  Read the current packet in net->buff */
    if (net_read_process_buffer(net, net_async->mp_state.mp_start_of_packet,
                                net_async->mp_state.mp_buf_length,
                                net_async->mp_state.mp_multi_byte_packet,
                                net_async->mp_state.mp_first_packet_offset))
      break;

    /*
      Read the mysql packet from vio, uncompress it and make it accessible
      through net->buff.
    */
    net_async->mp_state.mp_status = net_read_packet_nonblocking(net, &len);
    if (net_async->mp_state.mp_status == NET_ASYNC_NOT_READY) {
      net->save_char = net->buff[net_async->mp_state.mp_first_packet_offset];
      net->buf_length = net_async->mp_state.mp_buf_length;
      return net_async->mp_state.mp_status;
    }

    if (len == packet_error) {
      net_async->mp_state.mp_status = NET_ASYNC_COMPLETE;
      return net_async->mp_state.mp_status;
    }
    net_async->mp_state.mp_buf_length += len;
  }
  /*
    Once the packets are read in the net->buff, adjust the tracking offsets to
    the appropriate values.
  */
  len = net_read_update_offsets(net, net_async->mp_state.mp_start_of_packet,
                                net_async->mp_state.mp_first_packet_offset,
                                net_async->mp_state.mp_buf_length,
                                net_async->mp_state.mp_multi_byte_packet);
  net_async->mp_state.mp_status = NET_ASYNC_COMPLETE;
  return net_async->mp_state.mp_status;
}

/**
  Read the usual packet that is not compressed asynchronously.

  @param[in, out] net NET structure
  @param[out] len_ptr  length of the packet read

  @return status of the reads.
*/
static net_async_status net_read_uncompressed_nonblocking(NET *net,
                                                          ulong *len_ptr) {
  DBUG_TRACE;
  NET_ASYNC *net_async = NET_ASYNC_DATA(net);
  assert(!net->compress);
  ulong &len = *len_ptr;

  // Initialize the states
  if (net_async->mp_state.mp_status == NET_ASYNC_COMPLETE) {
    net_async->mp_state.mp_save_pos = net->where_b;
    net_async->mp_state.mp_total_length = 0;
  }

  net_async->mp_state.mp_status = net_read_packet_nonblocking(net, &len);
  net_async->mp_state.mp_total_length += len;
  net->where_b += len;

  if (len == MAX_PACKET_LENGTH)
    net_async->mp_state.mp_status = NET_ASYNC_NOT_READY;
  if (net_async->mp_state.mp_status == NET_ASYNC_NOT_READY)
    return net_async->mp_state.mp_status;

  // Update the offsets
  net->where_b = net_async->mp_state.mp_save_pos;
  len = net_async->mp_state.mp_total_length;
  net->read_pos = net->buff + net->where_b;
  net_async->mp_state.mp_status = NET_ASYNC_COMPLETE;
  return net_async->mp_state.mp_status;
}

/**
  Read one (variable-length) MySQL protocol packet.
  A MySQL packet consists of a header and a payload.

  @remark Reads one packet to net->buff + net->where_b.
  @remark Long packets are handled by my_net_read().
  @remark The network buffer is expanded if necessary.

  @return The length of the packet, or @c packet_error on error.
*/

static size_t net_read_packet(NET *net, size_t *complen) {
  size_t pkt_len, pkt_data_len;

  *complen = 0;

  net->reading_or_writing = 1;
#ifdef MYSQL_SERVER
  server_store_cached_values(net);
#endif

  /*
    We should reset compress_packet_nr even before reading the header because
    reading can fail and then the compressed packet number won't get reset.
  */
  net->compress_pkt_nr = net->pkt_nr;

  /* Retrieve packet length and number. */
  if (net_read_packet_header(net)) goto error;

  net->compress_pkt_nr = net->pkt_nr;

  if (net->compress) {
    /*
      The right-hand expression
      must match the size of the buffer allocated in net_realloc().
    */
    assert(net->where_b + NET_HEADER_SIZE + 3 <=
           net->max_packet + NET_HEADER_SIZE + COMP_HEADER_SIZE);

    /*
      If the packet is compressed then complen > 0 and contains the
      number of bytes in the uncompressed packet.
    */
    *complen = uint3korr(&(net->buff[net->where_b + NET_HEADER_SIZE]));
  }

  /* The length of the packet that follows. */
  pkt_len = uint3korr(net->buff + net->where_b);

  /* End of big multi-packet. */
  if (!pkt_len) goto end;

  pkt_data_len = max(pkt_len, *complen) + net->where_b;

  /* Expand packet buffer if necessary. */
  if ((pkt_data_len >= net->max_packet) && net_realloc(net, pkt_data_len))
    goto error;

  /* Read the packet data (payload). */
  if (net_read_raw_loop(net, pkt_len)) goto error;

end:
  if (net->error == NET_ERROR_SOCKET_NOT_WRITABLE)
    net->error = NET_ERROR_SOCKET_UNUSABLE;
  DBUG_DUMP("net read", net->buff + net->where_b, pkt_len);
  net->reading_or_writing = 0;
#ifdef MYSQL_SERVER
  server_store_cached_values(net);
#endif
  return pkt_len;

error:
  if (net->error == NET_ERROR_SOCKET_NOT_WRITABLE)
    net->error = NET_ERROR_SOCKET_UNUSABLE;
  net->reading_or_writing = 0;
#ifdef MYSQL_SERVER
  server_store_cached_values(net);
#endif
  return packet_error;
}

/*
  Non blocking version of my_net_read().
*/
net_async_status my_net_read_nonblocking(NET *net, ulong *len_ptr) {
  net_async_status status;
  if (net->compress)
    status = net_read_compressed_nonblocking(net, len_ptr);
  else
    status = net_read_uncompressed_nonblocking(net, len_ptr);

  if (status == NET_ASYNC_NOT_READY) return status;

  status = NET_ASYNC_COMPLETE;
  if (*len_ptr == packet_error) return status;

  DBUG_PRINT("info", ("chunk nb read: %lu", *len_ptr));
  return status;
}

/**
  Reads one packet to net->buff + net->where_b.
  If the packet is the first packet of a multi-packet packet
  (which is indicated by the length of the packet = 0xffffff) then
  all sub packets are read and concatenated.

  @param[in, out] net NET structure
  @param[out] len  length of the packet read
*/
static void net_read_uncompressed_packet(NET *net, size_t &len) {
  size_t complen;
  assert(!net->compress);
  len = net_read_packet(net, &complen);
  if (len == MAX_PACKET_LENGTH) {
    /* First packet of a multi-packet.  Concatenate the packets */
    const ulong save_pos = net->where_b;
    size_t total_length = 0;
    do {
      net->where_b += len;
      total_length += len;
      len = net_read_packet(net, &complen);
    } while (len == MAX_PACKET_LENGTH);
    if (len != packet_error) len += total_length;
    net->where_b = save_pos;
  }
  net->read_pos = net->buff + net->where_b;
  if (len != packet_error)
    net->read_pos[len] = 0; /* Safeguard for mysql_use_result */
}

/* We are using the compressed protocol */
static void net_read_compressed_packet(NET *net, size_t &len) {
  size_t complen;
  size_t start_of_packet;
  size_t first_packet_offset;
  size_t buf_length;
  uint multi_byte_packet = 0;
  assert(net->compress);
  net_read_init_offsets(net, start_of_packet, first_packet_offset,
                        multi_byte_packet, buf_length);
  for (;;) {
    if (net_read_process_buffer(net, start_of_packet, buf_length,
                                multi_byte_packet, first_packet_offset))
      break;

    if ((len = net_read_packet(net, &complen)) == packet_error) return;

    mysql_compress_context *mysql_compress_ctx = compress_context(net);
    if (my_uncompress(mysql_compress_ctx, net->buff + net->where_b, len,
                      &complen)) {
      net->error = NET_ERROR_SOCKET_UNUSABLE; /* caller will close socket */
      net->last_errno = ER_NET_UNCOMPRESS_ERROR;
#ifdef MYSQL_SERVER
      my_error(ER_NET_UNCOMPRESS_ERROR, MYF(0));
#endif
      len = packet_error;
      return;
    }
    buf_length += complen;
  }
  len = net_read_update_offsets(net, start_of_packet, first_packet_offset,
                                buf_length, multi_byte_packet);
}

/**
  Read a packet from the client/server and return it without the internal
  package header.

  If the packet is the first packet of a multi-packet packet
  (which is indicated by the length of the packet = 0xffffff) then
  all sub packets are read and concatenated.

  If the packet was compressed, its uncompressed and the length of the
  uncompressed packet is returned.

  @return
  The function returns the length of the found packet or packet_error.
  net->read_pos points to the read data.
*/

ulong my_net_read(NET *net) {
  size_t len;
  /* turn off non blocking operations */
  if (!vio_is_blocking(net->vio)) vio_set_blocking_flag(net->vio, true);

  if (net->compress)
    net_read_compressed_packet(net, len);
  else
    net_read_uncompressed_packet(net, len);

  return static_cast<ulong>(len);
}

void my_net_set_read_timeout(NET *net, uint timeout) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("timeout: %d", timeout));
  net->read_timeout = timeout;
  if (net->vio) vio_timeout(net->vio, 0, timeout);
}

void my_net_set_write_timeout(NET *net, uint timeout) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("timeout: %d", timeout));
  net->write_timeout = timeout;
  if (net->vio) vio_timeout(net->vio, 1, timeout);
}

void my_net_set_retry_count(NET *net, uint retry_count) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("retry_count: %d", retry_count));
  net->retry_count = retry_count;
  if (net->vio) net->vio->retry_count = retry_count;
}
