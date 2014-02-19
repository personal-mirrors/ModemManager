/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2008 - 2009 Novell, Inc.
 * Copyright (C) 2009 - 2010 Red Hat, Inc.
 */

#define _GNU_SOURCE  /* for strcasestr() */

#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <linux/serial.h>

#include <ModemManager.h>
#include <mm-errors-types.h>

#include "mm-serial-port.h"
#include "mm-log.h"

static gboolean mm_serial_port_queue_process (gpointer data);
static void mm_serial_port_close_force (MMSerialPort *self);

G_DEFINE_TYPE (MMSerialPort, mm_serial_port, MM_TYPE_PORT)

enum {
    PROP_0,
    PROP_BAUD,
    PROP_BITS,
    PROP_PARITY,
    PROP_STOPBITS,
    PROP_SEND_DELAY,
    PROP_FD,
    PROP_SPEW_CONTROL,
    PROP_RTS_CTS,
    PROP_FLASH_OK,

    LAST_PROP
};

enum {
    BUFFER_FULL,
    TIMED_OUT,
    FORCED_CLOSE,

    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

#define SERIAL_BUF_SIZE 2048

#define MM_SERIAL_PORT_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), MM_TYPE_SERIAL_PORT, MMSerialPortPrivate))

typedef struct {
    guint32 open_count;
    gboolean forced_close;
    int fd;
    GHashTable *reply_cache;
    GIOChannel *channel;
    GQueue *queue;
    GByteArray *response;

    struct termios old_t;

    guint baud;
    guint bits;
    char parity;
    guint stopbits;
    guint64 send_delay;
    gboolean spew_control;
    gboolean rts_cts;
    gboolean flash_ok;

    guint queue_id;
    guint watch_id;
    guint timeout_id;

    GCancellable *cancellable;
    gulong cancellable_id;

    guint n_consecutive_timeouts;

    guint flash_id;
    guint reopen_id;
    guint connected_id;
} MMSerialPortPrivate;

typedef struct {
    GByteArray *command;
    guint32 idx;
    guint32 eagain_count;
    gboolean started;
    gboolean done;
    GCallback callback;
    gpointer user_data;
    guint32 timeout;
    gboolean cached;
    GCancellable *cancellable;
} MMQueueData;

#if 0
static const char *
baud_to_string (int baud)
{
    const char *speed = NULL;

    switch (baud) {
    case B0:
        speed = "0";
        break;
    case B50:
        speed = "50";
        break;
    case B75:
        speed = "75";
        break;
    case B110:
        speed = "110";
        break;
    case B150:
        speed = "150";
        break;
    case B300:
        speed = "300";
        break;
    case B600:
        speed = "600";
        break;
    case B1200:
        speed = "1200";
        break;
    case B2400:
        speed = "2400";
        break;
    case B4800:
        speed = "4800";
        break;
    case B9600:
        speed = "9600";
        break;
    case B19200:
        speed = "19200";
        break;
    case B38400:
        speed = "38400";
        break;
    case B57600:
        speed = "57600";
        break;
    case B115200:
        speed = "115200";
        break;
    case B460800:
        speed = "460800";
        break;
    default:
        break;
    }

    return speed;
}

void
mm_serial_port_print_config (MMSerialPort *port, const char *detail)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (port);
    struct termios stbuf;
    int err;

    err = tcgetattr (priv->fd, &stbuf);
    if (err) {
        mm_warn ("*** %s (%s): (%s) tcgetattr() error %d",
                 __func__, detail, mm_port_get_device (MM_PORT (port)), errno);
        return;
    }

    mm_info ("(%s): (%s) baud rate: %d (%s)",
             detail, mm_port_get_device (MM_PORT (port)),
             stbuf.c_cflag & CBAUD,
             baud_to_string (stbuf.c_cflag & CBAUD));
}
#endif

static int
parse_baudrate (guint i)
{
    int speed;

    switch (i) {
    case 0:
        speed = B0;
        break;
    case 50:
        speed = B50;
        break;
    case 75:
        speed = B75;
        break;
    case 110:
        speed = B110;
        break;
    case 150:
        speed = B150;
        break;
    case 300:
        speed = B300;
        break;
    case 600:
        speed = B600;
        break;
    case 1200:
        speed = B1200;
        break;
    case 2400:
        speed = B2400;
        break;
    case 4800:
        speed = B4800;
        break;
    case 9600:
        speed = B9600;
        break;
    case 19200:
        speed = B19200;
        break;
    case 38400:
        speed = B38400;
        break;
    case 57600:
        speed = B57600;
        break;
    case 115200:
        speed = B115200;
        break;
    case 460800:
        speed = B460800;
        break;
    default:
        mm_warn ("Invalid baudrate '%d'", i);
        speed = B9600;
    }

    return speed;
}

static int
parse_bits (guint i)
{
    int bits;

    switch (i) {
    case 5:
        bits = CS5;
        break;
    case 6:
        bits = CS6;
        break;
    case 7:
        bits = CS7;
        break;
    case 8:
        bits = CS8;
        break;
    default:
        mm_warn ("Invalid bits (%d). Valid values are 5, 6, 7, 8.", i);
        bits = CS8;
    }

    return bits;
}

static int
parse_parity (char c)
{
    int parity;

    switch (c) {
    case 'n':
    case 'N':
        parity = 0;
        break;
    case 'e':
    case 'E':
        parity = PARENB;
        break;
    case 'o':
    case 'O':
        parity = PARENB | PARODD;
        break;
    default:
        mm_warn ("Invalid parity (%c). Valid values are n, e, o", c);
        parity = 0;
    }

    return parity;
}

static int
parse_stopbits (guint i)
{
    int stopbits;

    switch (i) {
    case 1:
        stopbits = 0;
        break;
    case 2:
        stopbits = CSTOPB;
        break;
    default:
        mm_warn ("Invalid stop bits (%d). Valid values are 1 and 2)", i);
        stopbits = 0;
    }

    return stopbits;
}

static gboolean
real_config_fd (MMSerialPort *self, int fd, GError **error)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);
    struct termios stbuf, other;
    int speed;
    int bits;
    int parity;
    int stopbits;

    speed = parse_baudrate (priv->baud);
    bits = parse_bits (priv->bits);
    parity = parse_parity (priv->parity);
    stopbits = parse_stopbits (priv->stopbits);

    memset (&stbuf, 0, sizeof (struct termios));
    if (tcgetattr (fd, &stbuf) != 0) {
        mm_warn ("(%s): tcgetattr() error: %d",
                 mm_port_get_device (MM_PORT (self)),
                 errno);
    }

    stbuf.c_iflag &= ~(IGNCR | ICRNL | IUCLC | INPCK | IXON | IXANY );
    stbuf.c_oflag &= ~(OPOST | OLCUC | OCRNL | ONLCR | ONLRET);
    stbuf.c_lflag &= ~(ICANON | XCASE | ECHO | ECHOE | ECHONL);
    stbuf.c_lflag &= ~(ECHO | ECHOE);
    stbuf.c_cc[VMIN] = 1;
    stbuf.c_cc[VTIME] = 0;
    stbuf.c_cc[VEOF] = 1;

    /* Use software handshaking and ignore parity/framing errors */
    stbuf.c_iflag |= (IXON | IXOFF | IXANY | IGNPAR);

    /* Set up port speed and serial attributes; also ignore modem control
     * lines since most drivers don't implement RTS/CTS anyway.
     */
    stbuf.c_cflag &= ~(CBAUD | CSIZE | CSTOPB | PARENB | CRTSCTS);
    stbuf.c_cflag |= (bits | CREAD | 0 | parity | stopbits | CLOCAL);

    errno = 0;
    if (cfsetispeed (&stbuf, speed) != 0) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "%s: failed to set serial port input speed; errno %d",
                     __func__, errno);
        return FALSE;
    }

    errno = 0;
    if (cfsetospeed (&stbuf, speed) != 0) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "%s: failed to set serial port output speed; errno %d",
                     __func__, errno);
        return FALSE;
    }

    if (tcsetattr (fd, TCSANOW, &stbuf) < 0) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "%s: failed to set serial port attributes; errno %d",
                     __func__, errno);
        return FALSE;
    }

    /* tcsetattr() returns 0 if any of the requested attributes could be set,
     * so we should double-check that all were set and log a warning if not.
     */
    memset (&other, 0, sizeof (struct termios));
    errno = 0;
    if (tcgetattr (fd, &other) != 0) {
        mm_warn ("(%s): tcgetattr() error: %d",
                 mm_port_get_device (MM_PORT (self)),
                 errno);
    }

    if (memcmp (&stbuf, &other, sizeof (other)) != 0) {
        mm_warn ("(%s): port attributes not fully set",
                 mm_port_get_device (MM_PORT (self)));
    }

    return TRUE;
}

static void
serial_debug (MMSerialPort *self, const char *prefix, const char *buf, gsize len)
{
    g_return_if_fail (len > 0);

    if (MM_SERIAL_PORT_GET_CLASS (self)->debug_log)
        MM_SERIAL_PORT_GET_CLASS (self)->debug_log (self, prefix, buf, len);
}

static gboolean
mm_serial_port_process_command (MMSerialPort *self,
                                MMQueueData *info,
                                GError **error)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);
    const guint8 *p;
    int status, expected_status, send_len;

    if (priv->fd < 0) {
        g_set_error_literal (error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_SEND_FAILED,
                             "Sending command failed: device is not enabled");
        return FALSE;
    }

    if (mm_port_get_connected (MM_PORT (self))) {
        g_set_error_literal (error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_SEND_FAILED,
                             "Sending command failed: device is connected");
        return FALSE;
    }

    /* Only print command the first time */
    if (info->started == FALSE) {
        info->started = TRUE;
        serial_debug (self, "-->", (const char *) info->command->data, info->command->len);
    }

    if (priv->send_delay == 0) {
        /* Send the whole command in one write */
        send_len = expected_status = info->command->len;
        p = info->command->data;
    } else {
        /* Send just one byte of the command */
        send_len = expected_status = 1;
        p = &info->command->data[info->idx];
    }

    /* Send a single byte of the command */
    errno = 0;
    status = write (priv->fd, p, send_len);
    if (status > 0)
        info->idx += status;
    else {
        /* Error or no bytes written */
        if (errno == EAGAIN || status == 0) {
            info->eagain_count--;
            if (info->eagain_count <= 0) {
                /* If we reach the limit of EAGAIN errors, treat as a timeout error. */
                priv->n_consecutive_timeouts++;
                g_signal_emit (self, signals[TIMED_OUT], 0, priv->n_consecutive_timeouts);

                g_set_error (error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_SEND_FAILED,
                             "Sending command failed: '%s'", strerror (errno));
                return FALSE;
            }
        } else {
            g_set_error (error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_SEND_FAILED,
                         "Sending command failed: '%s'", strerror (errno));
            return FALSE;
        }
    }

    if (info->idx >= info->command->len)
        info->done = TRUE;

    return TRUE;
}

static void
mm_serial_port_set_cached_reply (MMSerialPort *self,
                                 const GByteArray *command,
                                 const GByteArray *response)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);

    g_return_if_fail (self != NULL);
    g_return_if_fail (MM_IS_SERIAL_PORT (self));
    g_return_if_fail (command != NULL);

    if (response) {
        GByteArray *cmd_copy = g_byte_array_sized_new (command->len);
        GByteArray *rsp_copy = g_byte_array_sized_new (response->len);

        g_byte_array_append (cmd_copy, command->data, command->len);
        g_byte_array_append (rsp_copy, response->data, response->len);
        g_hash_table_insert (priv->reply_cache, cmd_copy, rsp_copy);
    } else
        g_hash_table_remove (MM_SERIAL_PORT_GET_PRIVATE (self)->reply_cache, command);
}

static const GByteArray *
mm_serial_port_get_cached_reply (MMSerialPort *self, GByteArray *command)
{
    return (const GByteArray *) g_hash_table_lookup (MM_SERIAL_PORT_GET_PRIVATE (self)->reply_cache, command);
}

static void
mm_serial_port_schedule_queue_process (MMSerialPort *self, guint timeout_ms)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);

    if (priv->timeout_id) {
        /* A command is already in progress */
        return;
    }

    if (priv->queue_id) {
        /* Already scheduled */
        return;
    }

    if (timeout_ms)
        priv->queue_id = g_timeout_add (timeout_ms, mm_serial_port_queue_process, self);
    else
        priv->queue_id = g_idle_add (mm_serial_port_queue_process, self);
}

static gsize
real_handle_response (MMSerialPort *self,
                      GByteArray *response,
                      GError *error,
                      GCallback callback,
                      gpointer callback_data)
{
    MMSerialResponseFn response_callback = (MMSerialResponseFn) callback;

    response_callback (self, response, error, callback_data);
    return response->len;
}

static void
mm_serial_port_got_response (MMSerialPort *self, GError *error)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);
    MMQueueData *info;
    gsize consumed = priv->response->len;

    if (priv->timeout_id) {
        g_source_remove (priv->timeout_id);
        priv->timeout_id = 0;
    }

    if (priv->cancellable_id) {
        g_assert (priv->cancellable != NULL);
        g_cancellable_disconnect (priv->cancellable,
                                  priv->cancellable_id);
        priv->cancellable_id = 0;
    }

    g_clear_object (&priv->cancellable);

    info = (MMQueueData *) g_queue_pop_head (priv->queue);
    if (info) {
        if (info->cached && !error)
            mm_serial_port_set_cached_reply (self, info->command, priv->response);

        if (info->callback) {
            g_warn_if_fail (MM_SERIAL_PORT_GET_CLASS (self)->handle_response != NULL);
            consumed = MM_SERIAL_PORT_GET_CLASS (self)->handle_response (self,
                                                                         priv->response,
                                                                         error,
                                                                         info->callback,
                                                                         info->user_data);
        }

        g_clear_object (&info->cancellable);
        g_byte_array_free (info->command, TRUE);
        g_slice_free (MMQueueData, info);
    }

    if (error)
        g_error_free (error);

    if (consumed)
        g_byte_array_remove_range (priv->response, 0, consumed);
    if (!g_queue_is_empty (priv->queue))
        mm_serial_port_schedule_queue_process (self, 0);
}

static gboolean
mm_serial_port_timed_out (gpointer data)
{
    MMSerialPort *self = MM_SERIAL_PORT (data);
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);
    GError *error;

    priv->timeout_id = 0;

    /* Update number of consecutive timeouts found */
    priv->n_consecutive_timeouts++;

    error = g_error_new_literal (MM_SERIAL_ERROR,
                                 MM_SERIAL_ERROR_RESPONSE_TIMEOUT,
                                 "Serial command timed out");

    /* FIXME: This is not completely correct - if the response finally arrives and there's
       some other command waiting for response right now, the other command will
       get the output of the timed out command. Not sure what to do here. */
    mm_serial_port_got_response (self, error);

    /* Emit a timed out signal, used by upper layers to identify a disconnected
     * serial port */
    g_signal_emit (self, signals[TIMED_OUT], 0, priv->n_consecutive_timeouts);

    return FALSE;
}

static void
serial_port_response_wait_cancelled (GCancellable *cancellable,
                                     MMSerialPort *self)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);
    GError *error;

    /* We don't want to call disconnect () while in the signal handler */
    priv->cancellable_id = 0;

    error = g_error_new_literal (MM_CORE_ERROR,
                                 MM_CORE_ERROR_CANCELLED,
                                 "Waiting for the reply cancelled");

    /* FIXME: This is not completely correct - if the response finally arrives and there's
       some other command waiting for response right now, the other command will
       get the output of the cancelled command. Not sure what to do here. */
    mm_serial_port_got_response (self, error);
}

static gboolean
mm_serial_port_queue_process (gpointer data)
{
    MMSerialPort *self = MM_SERIAL_PORT (data);
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);
    MMQueueData *info;
    GError *error = NULL;

    priv->queue_id = 0;

    info = (MMQueueData *) g_queue_peek_head (priv->queue);
    if (!info)
        return FALSE;

    if (info->cached) {
        const GByteArray *cached = mm_serial_port_get_cached_reply (self, info->command);

        if (cached) {
            /* Ensure the response array is fully empty before setting the
             * cached response.  */
            if (priv->response->len > 0) {
                mm_warn ("(%s) response array is not empty when using cached "
                         "reply, cleaning up %u bytes",
                         mm_port_get_device (MM_PORT (self)),
                         priv->response->len);
                g_byte_array_set_size (priv->response, 0);
            }

            g_byte_array_append (priv->response, cached->data, cached->len);
            mm_serial_port_got_response (self, NULL);
            return FALSE;
        }
    }

    if (mm_serial_port_process_command (self, info, &error)) {
        if (info->done) {
            /* setup the cancellable so that we can stop waiting for a response */
            if (info->cancellable) {
                priv->cancellable = g_object_ref (info->cancellable);
                priv->cancellable_id = (g_cancellable_connect (
                                            info->cancellable,
                                            (GCallback) serial_port_response_wait_cancelled,
                                            self,
                                            NULL));
                if (!priv->cancellable_id) {
                    error = g_error_new (MM_CORE_ERROR,
                                         MM_CORE_ERROR_CANCELLED,
                                         "Won't wait for the reply");
                    mm_serial_port_got_response (self, error);
                    return FALSE;
                }
            }

            /* If the command is finished being sent, schedule the timeout */
            priv->timeout_id = g_timeout_add_seconds (info->timeout,
                                                      mm_serial_port_timed_out,
                                                      self);
        } else {
            /* Schedule the next byte of the command to be sent */
            mm_serial_port_schedule_queue_process (self, priv->send_delay / 1000);
        }
    } else
        mm_serial_port_got_response (self, error);

    return FALSE;
}

static gboolean
parse_response (MMSerialPort *self,
                GByteArray *response,
                GError **error)
{
    if (MM_SERIAL_PORT_GET_CLASS (self)->parse_unsolicited)
        MM_SERIAL_PORT_GET_CLASS (self)->parse_unsolicited (self, response);

    g_return_val_if_fail (MM_SERIAL_PORT_GET_CLASS (self)->parse_response, FALSE);
    return MM_SERIAL_PORT_GET_CLASS (self)->parse_response (self, response, error);
}

static gboolean
data_available (GIOChannel *source,
                GIOCondition condition,
                gpointer data)
{
    MMSerialPort *self = MM_SERIAL_PORT (data);
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);
    char buf[SERIAL_BUF_SIZE + 1];
    gsize bytes_read;
    GIOStatus status;
    MMQueueData *info;
    const char *device;

    if (condition & G_IO_HUP) {
        device = mm_port_get_device (MM_PORT (self));
        mm_dbg ("(%s) unexpected port hangup!", device);

        if (priv->response->len)
            g_byte_array_remove_range (priv->response, 0, priv->response->len);
        mm_serial_port_close_force (self);
        return FALSE;
    }

    if (condition & G_IO_ERR) {
        if (priv->response->len)
            g_byte_array_remove_range (priv->response, 0, priv->response->len);
        return TRUE;
    }

    /* Don't read any input if the current command isn't done being sent yet */
    info = g_queue_peek_nth (priv->queue, 0);
    if (info && (info->started == TRUE) && (info->done == FALSE))
        return TRUE;

    do {
        GError *err = NULL;

        bytes_read = 0;
        status = g_io_channel_read_chars (source, buf, SERIAL_BUF_SIZE, &bytes_read, &err);
        if (status == G_IO_STATUS_ERROR) {
            if (err && err->message) {
                mm_warn ("(%s): read error: %s",
                         mm_port_get_device (MM_PORT (self)),
                         err->message);
            }
            g_clear_error (&err);
        }

        /* If no bytes read, just let g_io_channel wait for more data */
        if (bytes_read == 0)
            break;

        g_assert (bytes_read > 0);
        serial_debug (self, "<--", buf, bytes_read);
        g_byte_array_append (priv->response, (const guint8 *) buf, bytes_read);

        /* Make sure the response doesn't grow too long */
        if ((priv->response->len > SERIAL_BUF_SIZE) && priv->spew_control) {
            /* Notify listeners and then trim the buffer */
            g_signal_emit (self, signals[BUFFER_FULL], 0, priv->response);
            g_byte_array_remove_range (priv->response, 0, (SERIAL_BUF_SIZE / 2));
        }

        if (parse_response (self, priv->response, &err)) {
            /* Reset number of consecutive timeouts only here */
            priv->n_consecutive_timeouts = 0;
            mm_serial_port_got_response (self, err);
        }
    } while (   (bytes_read == SERIAL_BUF_SIZE || status == G_IO_STATUS_AGAIN)
             && (priv->watch_id > 0));

    return TRUE;
}

static void
data_watch_enable (MMSerialPort *self, gboolean enable)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);

    if (priv->watch_id) {
        if (enable)
            g_warn_if_fail (priv->watch_id == 0);

        g_source_remove (priv->watch_id);
        priv->watch_id = 0;
    }

    if (enable) {
        g_return_if_fail (priv->channel != NULL);
        priv->watch_id = g_io_add_watch (priv->channel,
                                         G_IO_IN | G_IO_ERR | G_IO_HUP,
                                         data_available, self);
    }
}

static void
port_connected (MMSerialPort *self, GParamSpec *pspec, gpointer user_data)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);
    gboolean connected;

    if (priv->fd < 0)
        return;

    /* When the port is connected, drop the serial port lock so PPP can do
     * something with the port.  When the port is disconnected, grab the lock
     * again.
     */
    connected = mm_port_get_connected (MM_PORT (self));

    if (ioctl (priv->fd, (connected ? TIOCNXCL : TIOCEXCL)) < 0) {
        mm_warn ("(%s): could not %s serial port lock: (%d) %s",
                 mm_port_get_device (MM_PORT (self)),
                 connected ? "drop" : "re-acquire",
                 errno,
                 strerror (errno));
        if (!connected) {
            // FIXME: do something here, maybe try again in a few seconds or
            // close the port and error out?
        }
    }

    /* When connected ignore let PPP have all the data */
    data_watch_enable (self, !connected);
}

gboolean
mm_serial_port_open (MMSerialPort *self, GError **error)
{
    MMSerialPortPrivate *priv;
    char *devfile;
    const char *device;
    struct serial_struct sinfo = { 0 };
    GTimeVal tv_start, tv_end;
    int errno_save = 0;

    g_return_val_if_fail (MM_IS_SERIAL_PORT (self), FALSE);

    priv = MM_SERIAL_PORT_GET_PRIVATE (self);
    device = mm_port_get_device (MM_PORT (self));

    if (priv->forced_close) {
        g_set_error (error,
                     MM_SERIAL_ERROR,
                     MM_SERIAL_ERROR_OPEN_FAILED,
                     "Could not open serial device %s: it has been forced close",
                     device);
        return FALSE;
    }

    if (priv->reopen_id) {
        g_set_error (error,
                     MM_SERIAL_ERROR,
                     MM_SERIAL_ERROR_OPEN_FAILED,
                     "Could not open serial device %s: reopen operation in progress",
                     device);
        return FALSE;
    }

    if (priv->open_count) {
        /* Already open */
        goto success;
    }

    mm_dbg ("(%s) opening serial port...", device);

    g_get_current_time (&tv_start);

    /* Only open a new file descriptor if we weren't given one already */
    if (priv->fd < 0) {
        devfile = g_strdup_printf ("/dev/%s", device);
        errno = 0;
        priv->fd = open (devfile, O_RDWR | O_EXCL | O_NONBLOCK | O_NOCTTY);
        errno_save = errno;
        g_free (devfile);
    }

    if (priv->fd < 0) {
        /* nozomi isn't ready yet when the port appears, and it'll return
         * ENODEV when open(2) is called on it.  Make sure we can handle this
         * by returning a special error in that case.
         */
        g_set_error (error,
                     MM_SERIAL_ERROR,
                     (errno == ENODEV) ? MM_SERIAL_ERROR_OPEN_FAILED_NO_DEVICE : MM_SERIAL_ERROR_OPEN_FAILED,
                     "Could not open serial device %s: %s", device, strerror (errno_save));
        mm_warn ("(%s) could not open serial device (%d)", device, errno_save);
        return FALSE;
    }

    if (ioctl (priv->fd, TIOCEXCL) < 0) {
        errno_save = errno;
        g_set_error (error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_OPEN_FAILED,
                     "Could not lock serial device %s: %s", device, strerror (errno_save));
        mm_warn ("(%s) could not lock serial device (%d)", device, errno_save);
        goto error;
    }

    /* Flush any waiting IO */
    tcflush (priv->fd, TCIOFLUSH);

    if (tcgetattr (priv->fd, &priv->old_t) < 0) {
        errno_save = errno;
        g_set_error (error, MM_SERIAL_ERROR, MM_SERIAL_ERROR_OPEN_FAILED,
                     "Could not set attributes on serial device %s: %s", device, strerror (errno_save));
        mm_warn ("(%s) could not set attributes on serial device (%d)", device, errno_save);
        goto error;
    }

    g_warn_if_fail (MM_SERIAL_PORT_GET_CLASS (self)->config_fd);
    if (!MM_SERIAL_PORT_GET_CLASS (self)->config_fd (self, priv->fd, error)) {
        mm_dbg ("(%s) failed to configure serial device", device);
        goto error;
    }

    /* Don't wait for pending data when closing the port; this can cause some
     * stupid devices that don't respond to URBs on a particular port to hang
     * for 30 seconds when probing fails.  See GNOME bug #630670.
     */
    if (ioctl (priv->fd, TIOCGSERIAL, &sinfo) == 0) {
        sinfo.closing_wait = ASYNC_CLOSING_WAIT_NONE;
        if (ioctl (priv->fd, TIOCSSERIAL, &sinfo) < 0)
            mm_warn ("(%s): couldn't set serial port closing_wait to none: %s",
                     device, g_strerror (errno));
    }

    g_get_current_time (&tv_end);

    if (tv_end.tv_sec - tv_start.tv_sec > 7)
        mm_warn ("(%s): open blocked by driver for more than 7 seconds!", device);

    priv->channel = g_io_channel_unix_new (priv->fd);
    g_io_channel_set_encoding (priv->channel, NULL, NULL);
    data_watch_enable (self, TRUE);

    g_warn_if_fail (priv->connected_id == 0);
    priv->connected_id = g_signal_connect (self, "notify::" MM_PORT_CONNECTED,
                                           G_CALLBACK (port_connected), NULL);

success:
    priv->open_count++;
    mm_dbg ("(%s) device open count is %d (open)", device, priv->open_count);

    /* Run additional port config if just opened */
    if (priv->open_count == 1 && MM_SERIAL_PORT_GET_CLASS (self)->config)
        MM_SERIAL_PORT_GET_CLASS (self)->config (self);

    return TRUE;

error:
    mm_warn ("(%s) failed to open serial device", device);
    close (priv->fd);
    priv->fd = -1;
    return FALSE;
}

gboolean
mm_serial_port_is_open (MMSerialPort *self)
{
    g_return_val_if_fail (self != NULL, FALSE);
    g_return_val_if_fail (MM_IS_SERIAL_PORT (self), FALSE);

    return !!MM_SERIAL_PORT_GET_PRIVATE (self)->open_count;
}

void
mm_serial_port_close (MMSerialPort *self)
{
    MMSerialPortPrivate *priv;
    const char *device;
    int i;

    g_return_if_fail (MM_IS_SERIAL_PORT (self));

    priv = MM_SERIAL_PORT_GET_PRIVATE (self);

    /* If we forced closing the port, open_count will be 0 already.
     * Just return without issuing any warning */
    if (priv->forced_close)
        return;

    g_return_if_fail (priv->open_count > 0);

    device = mm_port_get_device (MM_PORT (self));

    priv->open_count--;

    mm_dbg ("(%s) device open count is %d (close)", device, priv->open_count);

    if (priv->open_count > 0)
        return;

    if (priv->connected_id) {
        g_signal_handler_disconnect (self, priv->connected_id);
        priv->connected_id = 0;
    }

    mm_serial_port_flash_cancel (self);

    if (priv->fd >= 0) {
        GTimeVal tv_start, tv_end;
        struct serial_struct sinfo = { 0 };

        mm_dbg ("(%s) closing serial port...", device);

        mm_port_set_connected (MM_PORT (self), FALSE);

        /* Paranoid: ensure our closing_wait value is still set so we ignore
         * pending data when closing the port.  See GNOME bug #630670.
         */
        if (ioctl (priv->fd, TIOCGSERIAL, &sinfo) == 0) {
            if (sinfo.closing_wait != ASYNC_CLOSING_WAIT_NONE) {
                mm_warn ("(%s): serial port closing_wait was reset!", device);
                sinfo.closing_wait = ASYNC_CLOSING_WAIT_NONE;
                if (ioctl (priv->fd, TIOCSSERIAL, &sinfo) < 0)
                    mm_warn ("(%s): couldn't set serial port closing_wait to none: %s",
                             device, g_strerror (errno));
            }
        }

        g_get_current_time (&tv_start);

        if (priv->channel) {
            data_watch_enable (self, FALSE);
            g_io_channel_shutdown (priv->channel, TRUE, NULL);
            g_io_channel_unref (priv->channel);
            priv->channel = NULL;
        }

        tcsetattr (priv->fd, TCSANOW, &priv->old_t);
        tcflush (priv->fd, TCIOFLUSH);
        close (priv->fd);
        priv->fd = -1;

        g_get_current_time (&tv_end);

        mm_dbg ("(%s) serial port closed", device);

        /* Some ports don't respond to data and when close is called
         * the serial layer waits up to 30 second (closing_wait) for
         * that data to send before giving up and returning from close().
         * Log that.  See GNOME bug #630670 for more details.
         */
        if (tv_end.tv_sec - tv_start.tv_sec > 7)
            mm_warn ("(%s): close blocked by driver for more than 7 seconds!", device);
    }

    /* Clear the command queue */
    for (i = 0; i < g_queue_get_length (priv->queue); i++) {
        MMQueueData *item = g_queue_peek_nth (priv->queue, i);

        if (item->callback) {
            GError *error;
            GByteArray *response;

            g_warn_if_fail (MM_SERIAL_PORT_GET_CLASS (self)->handle_response != NULL);
            error = g_error_new_literal (MM_SERIAL_ERROR,
                                         MM_SERIAL_ERROR_SEND_FAILED,
                                         "Serial port is now closed");
            response = g_byte_array_sized_new (1);
            g_byte_array_append (response, (const guint8 *) "\0", 1);

            MM_SERIAL_PORT_GET_CLASS (self)->handle_response (self,
                                                              response,
                                                              error,
                                                              item->callback,
                                                              item->user_data);
            g_error_free (error);
            g_byte_array_free (response, TRUE);
        }

        g_clear_object (&item->cancellable);
        g_byte_array_free (item->command, TRUE);
        g_slice_free (MMQueueData, item);
    }
    g_queue_clear (priv->queue);

    if (priv->timeout_id) {
        g_source_remove (priv->timeout_id);
        priv->timeout_id = 0;
    }

    if (priv->queue_id) {
        g_source_remove (priv->queue_id);
        priv->queue_id = 0;
    }

    if (priv->cancellable_id) {
        g_assert (priv->cancellable != NULL);
        g_cancellable_disconnect (priv->cancellable,
                                  priv->cancellable_id);
        priv->cancellable_id = 0;
    }

    g_clear_object (&priv->cancellable);
}

static void
mm_serial_port_close_force (MMSerialPort *self)
{
    MMSerialPortPrivate *priv;

    g_return_if_fail (self != NULL);
    g_return_if_fail (MM_IS_SERIAL_PORT (self));

    priv = MM_SERIAL_PORT_GET_PRIVATE (self);

    /* If already forced to close, return */
    if (priv->forced_close)
        return;

    mm_dbg ("(%s) forced to close port", mm_port_get_device (MM_PORT (self)));

    /* If already closed, done */
    if (!priv->open_count)
        return;

    /* Force the port to close */
    priv->open_count = 1;
    mm_serial_port_close (self);

    /* Mark as having forced the close, so that we don't warn about incorrect
     * open counts */
    priv->forced_close = TRUE;

    /* Notify about the forced close status */
    g_signal_emit (self, signals[FORCED_CLOSE], 0);
}

static void
internal_queue_command (MMSerialPort *self,
                        GByteArray *command,
                        gboolean take_command,
                        gboolean cached,
                        guint32 timeout_seconds,
                        GCancellable *cancellable,
                        MMSerialResponseFn callback,
                        gpointer user_data)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);
    MMQueueData *info;

    g_return_if_fail (MM_IS_SERIAL_PORT (self));
    g_return_if_fail (command != NULL);

    if (priv->open_count == 0) {
        GError *error = g_error_new_literal (MM_SERIAL_ERROR,
                                             MM_SERIAL_ERROR_SEND_FAILED,
                                             "Sending command failed: device is not enabled");
        if (callback)
            callback (self, NULL, error, user_data);
        g_error_free (error);
        return;
    }

    info = g_slice_new0 (MMQueueData);
    if (take_command)
        info->command = command;
    else {
        info->command = g_byte_array_sized_new (command->len);
        g_byte_array_append (info->command, command->data, command->len);
    }

    /* Only accept about 3 seconds of EAGAIN for this command */
    if (priv->send_delay)
        info->eagain_count = 3000000 / priv->send_delay;
    else
        info->eagain_count = 1000;

    info->cached = cached;
    info->timeout = timeout_seconds;
    info->cancellable = (cancellable ? g_object_ref (cancellable) : NULL);
    info->callback = (GCallback) callback;
    info->user_data = user_data;

    /* Clear the cached value for this command if not asking for cached value */
    if (!cached)
        mm_serial_port_set_cached_reply (self, info->command, NULL);

    g_queue_push_tail (priv->queue, info);

    if (g_queue_get_length (priv->queue) == 1)
        mm_serial_port_schedule_queue_process (self, 0);
}

void
mm_serial_port_queue_command (MMSerialPort *self,
                              GByteArray *command,
                              gboolean take_command,
                              guint32 timeout_seconds,
                              GCancellable *cancellable,
                              MMSerialResponseFn callback,
                              gpointer user_data)
{
    internal_queue_command (self, command, take_command, FALSE, timeout_seconds, cancellable, callback, user_data);
}

void
mm_serial_port_queue_command_cached (MMSerialPort *self,
                                     GByteArray *command,
                                     gboolean take_command,
                                     guint32 timeout_seconds,
                                     GCancellable *cancellable,
                                     MMSerialResponseFn callback,
                                     gpointer user_data)
{
    internal_queue_command (self, command, take_command, TRUE, timeout_seconds, cancellable, callback, user_data);
}

typedef struct {
    MMSerialPort *port;
    guint initial_open_count;
    MMSerialReopenFn callback;
    gpointer user_data;
} ReopenInfo;

static void
serial_port_reopen_cancel (MMSerialPort *self)
{
    MMSerialPortPrivate *priv;

    g_return_if_fail (MM_IS_SERIAL_PORT (self));

    priv = MM_SERIAL_PORT_GET_PRIVATE (self);

    if (priv->reopen_id > 0) {
        g_source_remove (priv->reopen_id);
        priv->reopen_id = 0;
    }
}

static gboolean
reopen_do (gpointer data)
{
    ReopenInfo *info = (ReopenInfo *) data;
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (info->port);
    GError *error = NULL;
    guint i;

    priv->reopen_id = 0;

    for (i = 0; i < info->initial_open_count; i++) {
        if (!mm_serial_port_open (info->port, &error)) {
            g_prefix_error (&error, "Couldn't reopen port (%u): ", i);
            break;
        }
    }

    info->callback (info->port, error, info->user_data);
    if (error)
        g_error_free (error);
    g_slice_free (ReopenInfo, info);
    return FALSE;
}

gboolean
mm_serial_port_reopen (MMSerialPort *self,
                       guint32 reopen_time,
                       MMSerialReopenFn callback,
                       gpointer user_data)
{
    ReopenInfo *info;
    MMSerialPortPrivate *priv;
    guint i;

    g_return_val_if_fail (MM_IS_SERIAL_PORT (self), FALSE);
    priv = MM_SERIAL_PORT_GET_PRIVATE (self);

    if (priv->forced_close) {
        GError *error;

        error = g_error_new_literal (MM_CORE_ERROR,
                                     MM_CORE_ERROR_FAILED,
                                     "Serial port has been forced close.");
        callback (self, error, user_data);
        g_error_free (error);
        return FALSE;
    }

    if (priv->reopen_id > 0) {
        GError *error;

        error = g_error_new_literal (MM_CORE_ERROR,
                                     MM_CORE_ERROR_IN_PROGRESS,
                                     "Modem is already being reopened.");
        callback (self, error, user_data);
        g_error_free (error);
        return FALSE;
    }

    info = g_slice_new0 (ReopenInfo);
    info->port = self;
    info->callback = callback;
    info->user_data = user_data;

    priv = MM_SERIAL_PORT_GET_PRIVATE (self);
    info->initial_open_count = priv->open_count;

    mm_dbg ("(%s) reopening port (%u)",
            mm_port_get_device (MM_PORT (self)),
            info->initial_open_count);

    for (i = 0; i < info->initial_open_count; i++)
        mm_serial_port_close (self);

    if (reopen_time > 0)
        priv->reopen_id = g_timeout_add (reopen_time, reopen_do, info);
    else
        priv->reopen_id = g_idle_add (reopen_do, info);

    return TRUE;
}

static gboolean
get_speed (MMSerialPort *self, speed_t *speed, GError **error)
{
    struct termios options;

    memset (&options, 0, sizeof (struct termios));
    if (tcgetattr (MM_SERIAL_PORT_GET_PRIVATE (self)->fd, &options) != 0) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "%s: tcgetattr() error %d",
                     __func__, errno);
        return FALSE;
    }

    *speed = cfgetospeed (&options);
    return TRUE;
}

static gboolean
set_speed (MMSerialPort *self, speed_t speed, GError **error)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);
    struct termios options;
    int fd, count = 4;
    gboolean success = FALSE;

    fd = MM_SERIAL_PORT_GET_PRIVATE (self)->fd;

    memset (&options, 0, sizeof (struct termios));
    if (tcgetattr (fd, &options) != 0) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "%s: tcgetattr() error %d",
                     __func__, errno);
        return FALSE;
    }

    cfsetispeed (&options, speed);
    cfsetospeed (&options, speed);
    options.c_cflag |= (CLOCAL | CREAD);

    /* Configure flow control as well here */
    if (priv->rts_cts)
        options.c_cflag |= (CRTSCTS);

    while (count-- > 0) {
        if (tcsetattr (fd, TCSANOW, &options) == 0) {
            success = TRUE;
            break;  /* Operation successful */
        }

        /* Try a few times if EAGAIN */
        if (errno == EAGAIN)
            g_usleep (100000);
        else {
            /* If not EAGAIN, hard error */
            g_set_error (error,
                            MM_CORE_ERROR,
                            MM_CORE_ERROR_FAILED,
                            "%s: tcsetattr() error %d",
                            __func__, errno);
            return FALSE;
        }
    }

    if (!success) {
        g_set_error (error,
                        MM_CORE_ERROR,
                        MM_CORE_ERROR_FAILED,
                        "%s: tcsetattr() retry timeout",
                        __func__);
        return FALSE;
    }

    return TRUE;
}

typedef struct {
    MMSerialPort *port;
    speed_t current_speed;
    MMSerialFlashFn callback;
    gpointer user_data;
} FlashInfo;

static gboolean
flash_do (gpointer data)
{
    FlashInfo *info = (FlashInfo *) data;
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (info->port);
    GError *error = NULL;

    priv->flash_id = 0;

    if (priv->flash_ok) {
        if (info->current_speed) {
            if (!set_speed (info->port, info->current_speed, &error))
                g_assert (error);
        } else {
            error = g_error_new_literal (MM_SERIAL_ERROR,
                                         MM_SERIAL_ERROR_FLASH_FAILED,
                                         "Failed to retrieve current speed");
        }
    }

    info->callback (info->port, error, info->user_data);
    g_clear_error (&error);
    g_slice_free (FlashInfo, info);
    return FALSE;
}

gboolean
mm_serial_port_flash (MMSerialPort *self,
                      guint32 flash_time,
                      gboolean ignore_errors,
                      MMSerialFlashFn callback,
                      gpointer user_data)
{
    FlashInfo *info = NULL;
    MMSerialPortPrivate *priv;
    GError *error = NULL;
    gboolean success;

    g_return_val_if_fail (MM_IS_SERIAL_PORT (self), FALSE);
    g_return_val_if_fail (callback != NULL, FALSE);

    priv = MM_SERIAL_PORT_GET_PRIVATE (self);

    if (!mm_serial_port_is_open (self)) {
        error = g_error_new_literal (MM_SERIAL_ERROR,
                                     MM_SERIAL_ERROR_NOT_OPEN,
                                     "The serial port is not open.");
        goto error;
    }

    if (priv->flash_id > 0) {
        error = g_error_new_literal (MM_CORE_ERROR,
                                     MM_CORE_ERROR_IN_PROGRESS,
                                     "Modem is already being flashed.");
        goto error;
    }

    info = g_slice_new0 (FlashInfo);
    info->port = self;
    info->callback = callback;
    info->user_data = user_data;

    if (priv->flash_ok) {
        /* Grab current speed so we can reset it after flashing */
        success = get_speed (self, &info->current_speed, &error);
        if (!success && !ignore_errors)
            goto error;
        g_clear_error (&error);

        success = set_speed (self, B0, &error);
        if (!success && !ignore_errors)
            goto error;
        g_clear_error (&error);

        priv->flash_id = g_timeout_add (flash_time, flash_do, info);
    } else
        priv->flash_id = g_idle_add (flash_do, info);

    return TRUE;

error:
    callback (self, error, user_data);
    g_clear_error (&error);
    if (info)
        g_slice_free (FlashInfo, info);
    return FALSE;
}

void
mm_serial_port_flash_cancel (MMSerialPort *self)
{
    MMSerialPortPrivate *priv;

    g_return_if_fail (MM_IS_SERIAL_PORT (self));

    priv = MM_SERIAL_PORT_GET_PRIVATE (self);

    if (priv->flash_id > 0) {
        g_source_remove (priv->flash_id);
        priv->flash_id = 0;
    }
}

gboolean
mm_serial_port_get_flash_ok (MMSerialPort *self)
{
    g_return_val_if_fail (MM_IS_SERIAL_PORT (self), TRUE);

    return MM_SERIAL_PORT_GET_PRIVATE (self)->flash_ok;
}

/*****************************************************************************/

MMSerialPort *
mm_serial_port_new (const char *name, MMPortType ptype)
{
    return MM_SERIAL_PORT (g_object_new (MM_TYPE_SERIAL_PORT,
                                         MM_PORT_DEVICE, name,
                                         MM_PORT_SUBSYS, MM_PORT_SUBSYS_TTY,
                                         MM_PORT_TYPE, ptype,
                                         NULL));
}

static gboolean
ba_equal (gconstpointer v1, gconstpointer v2)
{
    const GByteArray *a = v1;
    const GByteArray *b = v2;

    if (!a && b)
        return -1;
    else if (a && !b)
        return 1;
    else if (!a && !b)
        return 0;

    g_assert (a && b);
    if (a->len < b->len)
        return -1;
    else if (a->len > b->len)
        return 1;

    g_assert (a->len == b->len);
    return !memcmp (a->data, b->data, a->len);
}

static guint
ba_hash (gconstpointer v)
{
    /* 31 bit hash function */
    const GByteArray *array = v;
    guint32 i, h = (const signed char) array->data[0];

    for (i = 1; i < array->len; i++)
        h = (h << 5) - h + (const signed char) array->data[i];

    return h;
}

static void
ba_free (gpointer v)
{
    g_byte_array_free ((GByteArray *) v, TRUE);
}

static void
mm_serial_port_init (MMSerialPort *self)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);

    priv->reply_cache = g_hash_table_new_full (ba_hash, ba_equal, ba_free, ba_free);

    priv->fd = -1;
    priv->baud = 57600;
    priv->bits = 8;
    priv->parity = 'n';
    priv->stopbits = 1;
    priv->send_delay = 1000;

    priv->queue = g_queue_new ();
    priv->response = g_byte_array_sized_new (500);
}

static void
set_property (GObject *object, guint prop_id,
              const GValue *value, GParamSpec *pspec)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (object);

    switch (prop_id) {
    case PROP_FD:
        priv->fd = g_value_get_int (value);
        break;
    case PROP_BAUD:
        priv->baud = g_value_get_uint (value);
        break;
    case PROP_BITS:
        priv->bits = g_value_get_uint (value);
        break;
    case PROP_PARITY:
        priv->parity = g_value_get_schar (value);
        break;
    case PROP_STOPBITS:
        priv->stopbits = g_value_get_uint (value);
        break;
    case PROP_SEND_DELAY:
        priv->send_delay = g_value_get_uint64 (value);
        break;
    case PROP_SPEW_CONTROL:
        priv->spew_control = g_value_get_boolean (value);
        break;
    case PROP_RTS_CTS:
        priv->rts_cts = g_value_get_boolean (value);
        break;
    case PROP_FLASH_OK:
        priv->flash_ok = g_value_get_boolean (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (object);

    switch (prop_id) {
    case PROP_FD:
        g_value_set_int (value, priv->fd);
        break;
    case PROP_BAUD:
        g_value_set_uint (value, priv->baud);
        break;
    case PROP_BITS:
        g_value_set_uint (value, priv->bits);
        break;
    case PROP_PARITY:
        g_value_set_schar (value, priv->parity);
        break;
    case PROP_STOPBITS:
        g_value_set_uint (value, priv->stopbits);
        break;
    case PROP_SEND_DELAY:
        g_value_set_uint64 (value, priv->send_delay);
        break;
    case PROP_SPEW_CONTROL:
        g_value_set_boolean (value, priv->spew_control);
        break;
    case PROP_RTS_CTS:
        g_value_set_boolean (value, priv->rts_cts);
        break;
    case PROP_FLASH_OK:
        g_value_set_boolean (value, priv->flash_ok);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
dispose (GObject *object)
{
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (object);

    if (priv->timeout_id) {
        g_source_remove (priv->timeout_id);
        priv->timeout_id = 0;
    }

    mm_serial_port_close_force (MM_SERIAL_PORT (object));

    serial_port_reopen_cancel (MM_SERIAL_PORT (object));
    mm_serial_port_flash_cancel (MM_SERIAL_PORT (object));

    G_OBJECT_CLASS (mm_serial_port_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
    MMSerialPort *self = MM_SERIAL_PORT (object);
    MMSerialPortPrivate *priv = MM_SERIAL_PORT_GET_PRIVATE (self);

    g_hash_table_destroy (priv->reply_cache);
    g_byte_array_free (priv->response, TRUE);
    g_queue_free (priv->queue);

    G_OBJECT_CLASS (mm_serial_port_parent_class)->finalize (object);
}

static void
mm_serial_port_class_init (MMSerialPortClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMSerialPortPrivate));

    /* Virtual methods */
    object_class->set_property = set_property;
    object_class->get_property = get_property;
    object_class->dispose = dispose;
    object_class->finalize = finalize;

    klass->config_fd = real_config_fd;
    klass->handle_response = real_handle_response;

    /* Properties */
    g_object_class_install_property
        (object_class, PROP_FD,
         g_param_spec_int (MM_SERIAL_PORT_FD,
                           "File descriptor",
                           "File descriptor",
                           -1, G_MAXINT, -1,
                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property
        (object_class, PROP_BAUD,
         g_param_spec_uint (MM_SERIAL_PORT_BAUD,
                            "Baud",
                            "Baud rate",
                            0, G_MAXUINT, 57600,
                            G_PARAM_READWRITE));

    g_object_class_install_property
        (object_class, PROP_BITS,
         g_param_spec_uint (MM_SERIAL_PORT_BITS,
                            "Bits",
                            "Bits",
                            5, 8, 8,
                            G_PARAM_READWRITE));

    g_object_class_install_property
        (object_class, PROP_PARITY,
         g_param_spec_char (MM_SERIAL_PORT_PARITY,
                            "Parity",
                            "Parity",
                            'E', 'o', 'n',
                            G_PARAM_READWRITE));

    g_object_class_install_property
        (object_class, PROP_STOPBITS,
         g_param_spec_uint (MM_SERIAL_PORT_STOPBITS,
                            "Stopbits",
                            "Stopbits",
                            1, 2, 1,
                            G_PARAM_READWRITE));

    g_object_class_install_property
        (object_class, PROP_SEND_DELAY,
         g_param_spec_uint64 (MM_SERIAL_PORT_SEND_DELAY,
                              "SendDelay",
                              "Send delay for each byte in microseconds",
                              0, G_MAXUINT64, 0,
                              G_PARAM_READWRITE));

    g_object_class_install_property
        (object_class, PROP_SPEW_CONTROL,
         g_param_spec_boolean (MM_SERIAL_PORT_SPEW_CONTROL,
                               "SpewControl",
                               "Spew control",
                               FALSE,
                               G_PARAM_READWRITE));

    g_object_class_install_property
        (object_class, PROP_RTS_CTS,
         g_param_spec_boolean (MM_SERIAL_PORT_RTS_CTS,
                               "RTSCTS",
                               "Enable RTS/CTS flow control",
                               FALSE,
                               G_PARAM_READWRITE));

    g_object_class_install_property
        (object_class, PROP_FLASH_OK,
         g_param_spec_boolean (MM_SERIAL_PORT_FLASH_OK,
                               "FlashOk",
                               "Flashing the port (0 baud for a short period) "
                               "is allowed.",
                               TRUE,
                               G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

    /* Signals */
    signals[BUFFER_FULL] =
        g_signal_new ("buffer-full",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (MMSerialPortClass, buffer_full),
                      NULL, NULL,
                      g_cclosure_marshal_generic,
                      G_TYPE_NONE, 1, G_TYPE_POINTER);

    signals[TIMED_OUT] =
        g_signal_new ("timed-out",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (MMSerialPortClass, timed_out),
                      NULL, NULL,
                      g_cclosure_marshal_generic,
					  G_TYPE_NONE, 1, G_TYPE_UINT);

    signals[FORCED_CLOSE] =
        g_signal_new ("forced-close",
                      G_OBJECT_CLASS_TYPE (object_class),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (MMSerialPortClass, forced_close),
                      NULL, NULL,
                      g_cclosure_marshal_generic,
                      G_TYPE_NONE, 0);
}
