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
 * Copyright (C) 2019 Xmm Semiconductor
 *
 * Author: Quincy Chen <Quincy.Chen@fibocom.com>
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-base-modem-at.h"
#include "mm-broadband-bearer-xmm-lte.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-log-object.h"
#include "mm-modem-helpers.h"
#include "mm-broadband-bearer.h"
#include "mm-modem-helpers-xmm.h"

#define CONNECTION_CHECK_TIMEOUT_SEC 5
#define STATCM_TAG "%STATCM:"
#define INVAILD_CID 255

G_DEFINE_TYPE (MMBroadbandBearerXmmLte, mm_broadband_bearer_xmm_lte, MM_TYPE_BROADBAND_BEARER);

/*****************************************************************************/
/* 3GPP IP config (sub-step of the 3GPP Connection sequence) */
typedef struct {
    MMBroadbandBearerXmmLte *self;
    MMBroadbandModem       *modem;
    MMPortSerialAt         *primary;
    MMPort                 *data;
    guint                   cid;
    gboolean                auth_required;
    /* For IPconfig settings */
    MMBearerIpConfig       *ipv4_config;
    MMBearerIpConfig       *ipv6_config;
} CommonConnectContext;

static void
common_connect_context_free (CommonConnectContext *ctx)
{
    if (ctx->ipv4_config)
        g_object_unref (ctx->ipv4_config);
    if (ctx->ipv6_config)
        g_object_unref (ctx->ipv6_config);
    if (ctx->data)
        g_object_unref (ctx->data);
    g_object_unref (ctx->self);
    g_object_unref (ctx->modem);
    g_object_unref (ctx->primary);
    g_slice_free (CommonConnectContext, ctx);
}

static GTask *
common_connect_task_new (MMBroadbandBearerXmmLte  *self,
                         MMBroadbandModem        *modem,
                         MMPortSerialAt          *primary,
                         guint                    cid,
                         MMPort                  *data,
                         GCancellable            *cancellable,
                         GAsyncReadyCallback      callback,
                         gpointer                 user_data)
{
    CommonConnectContext *ctx;
    GTask                *task;

    ctx = g_slice_new0 (CommonConnectContext);
    ctx->self    = g_object_ref (self);
    ctx->modem   = g_object_ref (modem);
    ctx->primary = g_object_ref (primary);
    ctx->cid     = cid;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify) common_connect_context_free);

    /* We need a net data port */
    if (data)
        ctx->data = g_object_ref (data);
    else {
        ctx->data = mm_base_modem_get_best_data_port (MM_BASE_MODEM (modem), MM_PORT_TYPE_NET);
        if (!ctx->data) {
            g_task_return_new_error (task,
                                     MM_CORE_ERROR,
                                     MM_CORE_ERROR_NOT_FOUND,
                                     "No valid data port found to launch connection");
            g_object_unref (task);
            return NULL;
        }
    }

    return task;
}

static gboolean
get_ip_config_3gpp_finish (MMBroadbandBearer *self,
                           GAsyncResult      *res,
                           MMBearerIpConfig **ipv4_config,
                           MMBearerIpConfig **ipv6_config,
                           GError           **error)
{
    MMBearerConnectResult *configs;
    MMBearerIpConfig *ipv4, *ipv6;

    configs = g_task_propagate_pointer (G_TASK (res), error);
    if (!configs)
        return FALSE;

    ipv4 = mm_bearer_connect_result_peek_ipv4_config (configs);
    ipv6 = mm_bearer_connect_result_peek_ipv6_config (configs);
    g_assert (ipv4 || ipv6);
    if (ipv4_config && ipv4)
        *ipv4_config = g_object_ref (ipv4);
    if (ipv6_config && ipv6)
        *ipv6_config = g_object_ref (ipv6);

    mm_bearer_connect_result_unref (configs);
    return TRUE;
}

static void
complete_get_ip_config_3gpp (GTask *task)
{
    CommonConnectContext *ctx;

    ctx = (CommonConnectContext *) g_task_get_task_data (task);
    g_assert (ctx->ipv4_config || ctx->ipv6_config);
    g_task_return_pointer (task,
                           mm_bearer_connect_result_new (ctx->data, ctx->ipv4_config, ctx->ipv6_config),
                           (GDestroyNotify) mm_bearer_connect_result_unref);
    g_object_unref (task);
}

static void
cgcontrdp_ready (MMBaseModem  *modem,
                 GAsyncResult *res,
                 GTask        *task)
{
    const gchar          *response;
    GError               *error = NULL;
    CommonConnectContext *ctx;
    guint     cid = G_MAXUINT;
    guint     bearer_id = G_MAXUINT;
    gchar    *apn = NULL;
    gchar    *local_address = NULL;
    guint    subnet = G_MAXUINT;
    gchar    *gateway_address = NULL;
    gchar    *dns_addresses[3] = { NULL, NULL, NULL };
    const gchar    *ipv6_info;
    MMBearerIpConfig *ip_config;

    ctx = (CommonConnectContext *) g_task_get_task_data (task);
    if(ctx->ipv6_config && !ctx->ipv4_config) 
        ip_config=ctx->ipv6_config; 
    else
        ip_config=ctx->ipv4_config;
    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response || !mm_xmm_parse_cgcontrdp_response (response,
                                                        (ip_config == ctx->ipv6_config?TRUE:FALSE),
                                                        &cid,
                                                        &bearer_id,
                                                        &apn,
                                                        &local_address,
                                                        &subnet,
                                                        &gateway_address,
                                                        &dns_addresses[0],
                                                        &dns_addresses[1],
                                                        &error)){
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    if (!subnet)
    {
    	subnet = 8;
    	mm_obj_dbg (modem,"ipv4 subnet donn't use get from modem");
    }
    mm_obj_dbg (modem,"IP address retrieved: %s", local_address);
    mm_bearer_ip_config_set_address (ip_config, local_address);
    mm_obj_dbg (modem,"IP subnet retrieved: %d", subnet);
    mm_bearer_ip_config_set_prefix (ip_config, subnet);
    mm_bearer_ip_config_set_gateway (ip_config, gateway_address);
    if (dns_addresses[0])
        mm_obj_dbg (modem,"Primary DNS retrieved: %s", dns_addresses[0]);

    if (dns_addresses[1])
        mm_obj_dbg (modem,"Secondary DNS retrieved: %s", dns_addresses[1]);

    mm_bearer_ip_config_set_dns (ip_config, (const gchar **) dns_addresses);

    g_free (apn);
    g_free (local_address);
    g_free (gateway_address);
    g_free (dns_addresses[0]);
    g_free (dns_addresses[1]);

    /*If there is ipv4V6 settings, the cgdcontrdp will return two lines*/
    if(ctx->ipv6_config && ctx->ipv4_config) {
        ipv6_info = 2+response;
        while(*ipv6_info != '\n')
            ipv6_info++;
    }
    else {
        mm_obj_dbg (modem,"finished IP settings retrieval for PDP context #%u...", ctx->cid);
        complete_get_ip_config_3gpp (task);
        return;
    }

    if (!mm_xmm_parse_cgcontrdp_response (ipv6_info,
                                            TRUE,
                                            &cid,
                                            &bearer_id,
                                            &apn,
                                            &local_address,
                                            &subnet,
                                            &gateway_address,
                                            &dns_addresses[0],
                                            &dns_addresses[1],
                                            &error)){
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_obj_dbg (modem,"IPv6 address retrieved: %s", local_address);
    mm_bearer_ip_config_set_address (ctx->ipv6_config, local_address);
    mm_obj_dbg (modem,"IPv6 subnet retrieved: %d", subnet);
    mm_bearer_ip_config_set_prefix (ctx->ipv6_config,subnet);
    mm_bearer_ip_config_set_gateway (ctx->ipv6_config, gateway_address);
    if (dns_addresses[0])
        mm_obj_dbg (modem,"Primary DNS retrieved: %s", dns_addresses[0]);

    if (dns_addresses[1])
        mm_obj_dbg (modem,"Secondary DNS retrieved: %s", dns_addresses[1]);

    mm_bearer_ip_config_set_dns (ctx->ipv6_config, (const gchar **) dns_addresses);

    g_free (apn);
    g_free (local_address);
    g_free (gateway_address);
    g_free (dns_addresses[0]);
    g_free (dns_addresses[1]);
    mm_obj_dbg (modem,"finished IPV4V6 settings retrieval for PDP context #%u...", ctx->cid);

    complete_get_ip_config_3gpp (task);
}

static void
get_ip_config_3gpp (MMBroadbandBearer   *self,
                    MMBroadbandModem    *modem,
                    MMPortSerialAt      *primary,
                    MMPortSerialAt      *secondary,
                    MMPort              *data,
                    guint                cid,
                    MMBearerIpFamily     ip_family,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
    GTask                *task;
    CommonConnectContext *ctx;

    if (!(task = common_connect_task_new (MM_BROADBAND_BEARER_XMM_LTE (self),
                                          MM_BROADBAND_MODEM (modem),
                                          primary,
                                          cid,
                                          data,
                                          NULL,
                                          callback,
                                          user_data)))
        return;

    ctx = (CommonConnectContext *) g_task_get_task_data (task);
    if (ip_family & MM_BEARER_IP_FAMILY_IPV4 || ip_family & MM_BEARER_IP_FAMILY_IPV4V6) {
        ctx->ipv4_config = mm_bearer_ip_config_new ();
        mm_bearer_ip_config_set_method (ctx->ipv4_config, MM_BEARER_IP_METHOD_STATIC);
    }
    if (ip_family & MM_BEARER_IP_FAMILY_IPV6 || ip_family & MM_BEARER_IP_FAMILY_IPV4V6) {
        ctx->ipv6_config = mm_bearer_ip_config_new ();
        mm_bearer_ip_config_set_method (ctx->ipv6_config, MM_BEARER_IP_METHOD_STATIC);
    }

    /* As we not support DHCP, we need to ask for static IP addressing details:
     *  - +CGCONTRDP?[CID] will give us the IP address, subnet and DNS addresses.
     */
    if (1) {
        gchar *cmd;
	if (cid >= 31) {
        cmd = g_strdup_printf ("+CGCONTRDP=%u", 0);
	} else {
        cmd = g_strdup_printf ("+CGCONTRDP=%u", cid);
	}
        mm_obj_dbg (self, "gathering gateway information for PDP context #%u...", cid);
        mm_base_modem_at_command (MM_BASE_MODEM (modem),
                                  cmd,
                                  10,
                                  FALSE,
                                  (GAsyncReadyCallback) cgcontrdp_ready,
                                  task);
        g_free (cmd);
        return;
    }

    g_assert_not_reached ();
}

/*****************************************************************************/
/* 3GPP Dialing (sub-step of the 3GPP Connection sequence) */
typedef enum {
    DIAL_3GPP_STEP_FIRST,
    DIAL_3GPP_STEP_XDNS,
    DIAL_3GPP_STEP_PDP_ACT,
    DIAL_3GPP_STEP_DATA_CHANNEL,
    DIAL_3GPP_STEP_CONNECT,
    DIAL_3GPP_STEP_LAST
} Dial3gppStep;

typedef enum {
    IPV4 = 0x01,
    IPV6 = 0x02,
} IpTypeMask;

typedef struct {
    MMBaseModem *modem;
    MMPortSerialAt *primary;
    MMPort *data;
    guint cid;
    Dial3gppStep step;
    MMBearerIpFamily   ip_family;
} Dial3gppContext;

static void
dial_3gpp_context_free (Dial3gppContext *ctx)
{
    if (ctx->data)
        g_object_unref (ctx->data);
    g_object_unref (ctx->primary);
    g_object_unref (ctx->modem);
    g_slice_free (Dial3gppContext, ctx);
}

static MMPort *
dial_3gpp_finish (MMBroadbandBearer *self,
                  GAsyncResult *res,
                  GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}


static void dial_3gpp_context_step (GTask *task);


static void
data_mode_ready (MMBaseModem *modem,
                    GAsyncResult *res,
                    GTask *task)
{
    Dial3gppContext *ctx;
    GError *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_full_finish (modem, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Go on */
    ctx->step++;
    dial_3gpp_context_step (task);
}

static void
channel_set_ready (MMBaseModem *modem,
             GAsyncResult *res,
             GTask *task)
{
    Dial3gppContext *ctx;
    GError *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_full_finish (modem, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Go on */
    ctx->step++;
    dial_3gpp_context_step (task);
}

static void
dial_3gpp_context_step (GTask *task)
{
    MMBroadbandBearerXmmLte *self;
    Dial3gppContext *ctx;
    gchar *command;
    guint xdns_ip_type;
    
    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    switch (ctx->step) {
    case DIAL_3GPP_STEP_FIRST:
        if (ctx->cid == 0) {
            ctx->step = DIAL_3GPP_STEP_DATA_CHANNEL;
            dial_3gpp_context_step (task);
            return;
        }
        else
            /* Fall down */
            ctx->step++;
    
    case DIAL_3GPP_STEP_XDNS:
        if (ctx->ip_family == MM_BEARER_IP_FAMILY_IPV4)
            xdns_ip_type = 1;
        else if (ctx->ip_family == MM_BEARER_IP_FAMILY_IPV6)
            xdns_ip_type = 2;
        else if (ctx->ip_family == MM_BEARER_IP_FAMILY_IPV4V6)
            xdns_ip_type = 3;
        else 
            xdns_ip_type = 0;

        if(xdns_ip_type & IPV4) {
            if (ctx->cid >= 31) {
            	command = g_strdup_printf ("+XDNS=%d,%d", 0,IPV4);
	    } else { 
            	command = g_strdup_printf ("+XDNS=%d,%d",ctx->cid,IPV4);
	    }
            mm_base_modem_at_command_full (ctx->modem,
                                           ctx->primary,
                                           command,
                                           3,
                                           FALSE,
                                           FALSE, /* raw */
                                           NULL, /* cancellable */
                                           NULL,
                                           NULL);
                g_free (command);
        }

        if(xdns_ip_type & IPV6) {
	    if (ctx->cid >= 31) {
            	command = g_strdup_printf ("+XDNS=%d,%d", 0,IPV6);
	    } else {
            	command = g_strdup_printf ("+XDNS=%d,%d",ctx->cid,IPV6);
	    }
            mm_base_modem_at_command_full (ctx->modem,
                                           ctx->primary,
                                           command,
                                           3,
                                           FALSE,
                                           FALSE, /* raw */
                                           NULL, /* cancellable */
                                           NULL,
                                           NULL);
            g_free (command);

            /* in ipv6 case, we should set ipv6 format*/
            /*
            *"+CGPIAF=1,1,0,1"
            *1 Use IPv6-like colon-notation.
            *1 The printout format is applying / (forward slash) subnet-prefix Classless Inter-Domain
            *   Routing (CIDR) notation.
            *   Example: "2001:0DB8:0000:CD30:0000:0000:0000:0000/60"
            *0 Leading zeros are omitted.
            *1 Use zero compression.
            */
            mm_base_modem_at_command_full (ctx->modem,
                                           ctx->primary,
                                           "+CGPIAF=1,1,0,1",
                                           3,
                                           FALSE,
                                           FALSE, /* raw */
                                           NULL, /* cancellable */
                                           NULL,
                                           NULL);            
        }
        /* Fall down */
        ctx->step++;
        
    case DIAL_3GPP_STEP_PDP_ACT:
	if (ctx->cid >= 31) {
        	command = g_strdup_printf ("+CGACT=1,%d", 0);
	} else {
        	command = g_strdup_printf ("+CGACT=1,%d",ctx->cid);
	}
        mm_base_modem_at_command_full (ctx->modem,
                                       ctx->primary,
                                       command,
                                       5,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       NULL,
                                       NULL);
        g_free (command);
        /* Fall down */
        ctx->step++;
                
    case DIAL_3GPP_STEP_DATA_CHANNEL:
       //command = g_strdup_printf ("+CGACT=1,%d",ctx->cid);
	if (ctx->cid >= 31) {
        	command = g_strdup_printf ("+CGACT=1,%d", 0);
	} else {
        	command = g_strdup_printf ("+CGACT=1,%d",ctx->cid);
	}
        mm_base_modem_at_command_full (ctx->modem,
                                       ctx->primary,
                                       command,
                                       5,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       NULL,
                                       NULL);
 	 if (ctx->cid >= 31) {
        	command = g_strdup_printf ("+xdatachannel=1,1,\"/PCIE/IOSM/CTRL/1\",\"/PCIE/IOSM/IPS/0\",2,%d", 0);
	 } else { 
        	command = g_strdup_printf ("+xdatachannel=1,1,\"/PCIE/IOSM/CTRL/1\",\"/PCIE/IOSM/IPS/0\",2,%d",ctx->cid);
	 }
        mm_base_modem_at_command_full (ctx->modem,
                                       ctx->primary,
                                       command,
                                       10,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)channel_set_ready,
                                       task);
            g_free (command);
            return;

    case DIAL_3GPP_STEP_CONNECT:
	if (ctx->cid >= 31) {
        command = g_strdup_printf ("+CGDATA=\"M-RAW_IP\",%d", 0);
	} else {
        command = g_strdup_printf ("+CGDATA=\"M-RAW_IP\",%d",ctx->cid);
	}
        mm_base_modem_at_command_full (ctx->modem,
                                       ctx->primary,
                                       command,
                                       10,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)data_mode_ready,
                                       task);
            g_free (command);
            return;

    case DIAL_3GPP_STEP_LAST:
        g_task_return_pointer (task,
                               g_object_ref (ctx->data),
                               g_object_unref);
        g_object_unref (task);
        return;
    }
}

static void
dial_3gpp (MMBroadbandBearer *self,
           MMBaseModem *modem,
           MMPortSerialAt *primary,
           guint cid,
           GCancellable *cancellable,
           GAsyncReadyCallback callback,
           gpointer user_data)
{
    Dial3gppContext *ctx;
    GTask *task;

    g_assert (primary != NULL);

    ctx = g_slice_new0 (Dial3gppContext);
    ctx->modem = g_object_ref (modem);
    ctx->primary = g_object_ref (primary);
    ctx->cid = cid;
    ctx->step = DIAL_3GPP_STEP_FIRST;
    ctx->ip_family = mm_bearer_properties_get_ip_type (mm_base_bearer_peek_config (MM_BASE_BEARER (self)));
    
    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify)dial_3gpp_context_free);

    /* Get a 'net' data port */
    ctx->data = mm_base_modem_get_best_data_port (ctx->modem, MM_PORT_TYPE_NET);
    if (!ctx->data) {
        g_task_return_new_error (
            task,
            MM_CORE_ERROR,
            MM_CORE_ERROR_NOT_FOUND,
            "Couldn't connect: no available net port available");
        g_object_unref (task);
        return;
    }

    dial_3gpp_context_step (task);
}

/*****************************************************************************/
/* 3GPP disconnect */

static gboolean
disconnect_3gpp_finish (MMBroadbandBearer *self,
                        GAsyncResult *res,
                        GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_disconnect_3gpp_ready (MMBroadbandBearer *self,
                              GAsyncResult *res,
                              GTask *task)
{
    GError *error = NULL;

    if (!MM_BROADBAND_BEARER_CLASS (mm_broadband_bearer_xmm_lte_parent_class)->disconnect_3gpp_finish (self, res, &error)) {
        mm_obj_dbg (self,"Parent disconnection failed (not fatal): %s", error->message);
        g_error_free (error);
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
disconnect_cgact_ready (MMBaseModem *modem,
                        GAsyncResult *res,
                        GTask *task)
{
    GError *error = NULL;

    /* Ignore errors for now */
    mm_base_modem_at_command_full_finish (modem, res, &error);
    if (error) {
        mm_obj_dbg (modem, "Disconnection failed (not fatal): %s", error->message);
        g_error_free (error);
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
disconnect_3gpp (MMBroadbandBearer *self,
                 MMBroadbandModem *modem,
                 MMPortSerialAt *primary,
                 MMPortSerialAt *secondary,
                 MMPort *data,
                 guint cid,
                 GAsyncReadyCallback callback,
                 gpointer user_data)
{
    GTask *task;

    g_assert (primary != NULL);

    task = g_task_new (self, NULL, callback, user_data);

    if (!MM_IS_PORT_SERIAL_AT (data)) {
        gchar *command;

        /* Use specific CID */
        //command =(cid > 0?  g_strdup_printf ("+CGACT=0,%u", cid): g_strdup_printf ("+CGACT=0"));
	//command =  g_strdup_printf ("+CGACT=0");
	if (cid >= 31) {
	command =  g_strdup_printf ("+CGACT=0");
	} else {
	command =  g_strdup_printf ("+CGACT=0,%u",cid);
	}
        mm_base_modem_at_command_full (MM_BASE_MODEM (modem),
                                       primary,
                                       command,
                                       10,
                                       FALSE,
                                       FALSE, /* raw */
                                       NULL, /* cancellable */
                                       (GAsyncReadyCallback)disconnect_cgact_ready,
                                       task);
        g_free (command);
        return;
    }

    /* Chain up parent's disconnection if we don't have a net port */
    MM_BROADBAND_BEARER_CLASS (mm_broadband_bearer_xmm_lte_parent_class)->disconnect_3gpp (
        self,
        modem,
        primary,
        secondary,
        data,
        cid,
        (GAsyncReadyCallback)parent_disconnect_3gpp_ready,
        task);
}


/*****************************************************************************/

MMBaseBearer *
mm_broadband_bearer_xmm_lte_new_finish (GAsyncResult *res,
                                           GError **error)
{
    GObject *bearer;
    GObject *source;

    source = g_async_result_get_source_object (res);
    bearer = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, error);
    g_object_unref (source);
    //cklog();

    if (!bearer)
        return NULL;

    /* Only export valid bearers */
    mm_base_bearer_export (MM_BASE_BEARER (bearer));

    return MM_BASE_BEARER (bearer);
}

void
mm_broadband_bearer_xmm_lte_new (MMBroadbandModemXmm *modem,
                                    MMBearerProperties *config,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
    g_async_initable_new_async (
        MM_TYPE_BROADBAND_BEARER_XMM_LTE,
        G_PRIORITY_DEFAULT,
        cancellable,
        callback,
        user_data,
        MM_BASE_BEARER_MODEM, modem,
        MM_BASE_BEARER_CONFIG, config,
        NULL);
}

static void
mm_broadband_bearer_xmm_lte_init (MMBroadbandBearerXmmLte *self)
{

}

static void
mm_broadband_bearer_xmm_lte_class_init (MMBroadbandBearerXmmLteClass *klass)
{

    MMBroadbandBearerClass *broadband_bearer_class = MM_BROADBAND_BEARER_CLASS (klass);

    broadband_bearer_class->dial_3gpp= dial_3gpp;
    broadband_bearer_class->dial_3gpp_finish = dial_3gpp_finish;
    broadband_bearer_class->disconnect_3gpp = disconnect_3gpp;
    broadband_bearer_class->disconnect_3gpp_finish = disconnect_3gpp_finish;

    broadband_bearer_class->get_ip_config_3gpp = get_ip_config_3gpp;
    broadband_bearer_class->get_ip_config_3gpp_finish = get_ip_config_3gpp_finish;

}
