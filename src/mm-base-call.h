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
 * Copyright (C) 2015 Riccardo Vangelisti <riccardo.vangelisti@sadel.it>
 */

#ifndef MM_BASE_CALL_H
#define MM_BASE_CALL_H

#include <glib.h>
#include <glib-object.h>

#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-base-modem.h"
#include "mm-call-audio-format.h"

#define MM_TYPE_BASE_CALL            (mm_base_call_get_type ())
#define MM_BASE_CALL(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BASE_CALL, MMBaseCall))
#define MM_BASE_CALL_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_BASE_CALL, MMBaseCallClass))
#define MM_IS_BASE_CALL(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BASE_CALL))
#define MM_IS_BASE_CALL_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_BASE_CALL))
#define MM_BASE_CALL_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_BASE_CALL, MMBaseCallClass))

typedef struct _MMBaseCall MMBaseCall;
typedef struct _MMBaseCallClass MMBaseCallClass;
typedef struct _MMBaseCallPrivate MMBaseCallPrivate;

#define MM_BASE_CALL_PATH                        "call-path"
#define MM_BASE_CALL_CONNECTION                  "call-connection"
#define MM_BASE_CALL_MODEM                       "call-modem"
#define MM_BASE_CALL_SUPPORTS_DIALING_TO_RINGING "call-supports-dialing-to-ringing"
#define MM_BASE_CALL_SUPPORTS_RINGING_TO_ACTIVE  "call-supports-ringing-to-active"

struct _MMBaseCall {
    MmGdbusCallSkeleton parent;
    MMBaseCallPrivate *priv;
};

struct _MMBaseCallClass {
    MmGdbusCallSkeletonClass parent;

    /* Start the call */
    void     (* start)        (MMBaseCall *self,
                               GAsyncReadyCallback callback,
                               gpointer user_data);
    gboolean (* start_finish) (MMBaseCall *self,
                               GAsyncResult *res,
                               GError **error);

    /* Accept the call */
    void     (* accept)        (MMBaseCall *self,
                                GAsyncReadyCallback callback,
                                gpointer user_data);
    gboolean (* accept_finish) (MMBaseCall *self,
                                GAsyncResult *res,
                                GError **error);

    /* Hangup the call */
    void     (* hangup)        (MMBaseCall *self,
                                GAsyncReadyCallback callback,
                                gpointer user_data);
    gboolean (* hangup_finish) (MMBaseCall *self,
                                GAsyncResult *res,
                                GError **error);

    /* Send a DTMF tone */
    void     (* send_dtmf)        (MMBaseCall *self,
                                   const gchar *dtmf,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data);
    gboolean (* send_dtmf_finish) (MMBaseCall *self,
                                   GAsyncResult *res,
                                   GError **error);

    /* Setup/cleanup in-call unsolicited events */
    gboolean (* setup_unsolicited_events)   (MMBaseCall  *self,
                                             GError     **error);
    gboolean (* cleanup_unsolicited_events) (MMBaseCall  *self,
                                             GError     **error);

    /* Setup/cleanup audio channel */
    void     (* setup_audio_channel)          (MMBaseCall           *self,
                                               GAsyncReadyCallback   callback,
                                               gpointer              user_data);
    gboolean (* setup_audio_channel_finish)   (MMBaseCall           *self,
                                               GAsyncResult         *res,
                                               MMPort              **audio_port,
                                               MMCallAudioFormat   **audio_format,
                                               GError              **error);
    void     (* cleanup_audio_channel)        (MMBaseCall           *self,
                                               GAsyncReadyCallback   callback,
                                               gpointer              user_data);
    gboolean (* cleanup_audio_channel_finish) (MMBaseCall           *self,
                                               GAsyncResult         *res,
                                               GError              **error);
};

GType mm_base_call_get_type (void);

/* This one can be overriden by plugins */
MMBaseCall *mm_base_call_new (MMBaseModem     *modem,
                              MMCallDirection  direction,
                              const gchar     *number);

void         mm_base_call_export   (MMBaseCall *self);
void         mm_base_call_unexport (MMBaseCall *self);
const gchar *mm_base_call_get_path (MMBaseCall *self);

void         mm_base_call_change_state (MMBaseCall *self,
                                        MMCallState new_state,
                                        MMCallStateReason reason);

void         mm_base_call_received_dtmf (MMBaseCall *self,
                                         const gchar *dtmf);

void         mm_base_call_incoming_refresh (MMBaseCall *self);

#endif /* MM_BASE_CALL_H */
