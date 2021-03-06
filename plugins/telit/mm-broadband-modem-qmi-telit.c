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
 * Copyright (C) 2009 - 2012 Red Hat, Inc.
 * Copyright (C) 2012 Aleksander Morgado <aleksander@gnu.org>
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#define MM_LOG_NO_OBJECT
#include "ModemManager.h"
#include "mm-log.h"
#include "mm-errors-types.h"
#include "mm-modem-helpers.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem.h"
#include "mm-broadband-modem-qmi-telit.h"
#include "mm-modem-helpers-telit.h"
#include "mm-telit-enums-types.h"

static void iface_modem_init (MMIfaceModem *iface);

static MMIfaceModem *iface_modem_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemQmiTelit, mm_broadband_modem_qmi_telit, MM_TYPE_BROADBAND_MODEM_QMI, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init));

typedef enum {
    FEATURE_SUPPORT_UNKNOWN,
    FEATURE_NOT_SUPPORTED,
    FEATURE_SUPPORTED
} FeatureSupport;

struct _MMBroadbandModemQmiTelitPrivate {
    MMTelitQssStatus qss_status;
    gint qss_active_slot;
};

/*****************************************************************************/
/* Setup SIM hot swap (Modem interface) */

typedef enum {
    QSS_SETUP_STEP_FIRST,
    QSS_SETUP_STEP_QUERY,
    QSS_SETUP_STEP_ENABLE_PRIMARY_PORT,
    QSS_SETUP_STEP_ENABLE_SECONDARY_PORT,
    QSS_SETUP_STEP_LAST
} QssSetupStep;

typedef struct {
    QssSetupStep step;
    MMPortSerialAt *primary;
    MMPortSerialAt *secondary;
    GError *primary_error;
    GError *secondary_error;
} QssSetupContext;

static void qss_setup_step (GTask *task);

static void
telit_qss_unsolicited_handler (MMPortSerialAt *port,
                               GMatchInfo *match_info,
                               MMBroadbandModemQmiTelit *self)
{
    MMTelitQssStatus cur_qss_status;
    MMTelitQssStatus prev_qss_status;
    gint cur_qss_active_slot;
    gint prev_qss_active_slot;

    if (!mm_get_int_from_match_info (match_info, 1, (gint*)&cur_qss_status))
        return;
    mm_get_int_from_match_info (match_info, 2, &cur_qss_active_slot);

    prev_qss_status = self->priv->qss_status;
    self->priv->qss_status = cur_qss_status;

    prev_qss_active_slot = self->priv->qss_active_slot;
    self->priv->qss_active_slot = cur_qss_active_slot;

    if (cur_qss_status != prev_qss_status)
        mm_dbg ("QSS handler: status changed '%s -> %s'",
                mm_telit_qss_status_get_string (prev_qss_status),
                mm_telit_qss_status_get_string (cur_qss_status));
    if (cur_qss_active_slot != prev_qss_active_slot)
        mm_dbg ("QSS handler: active slot changed '%d -> %d'",
                prev_qss_active_slot, cur_qss_active_slot);

    /* FIXME: when the active slot parameter */
    if ((prev_qss_status == QSS_STATUS_SIM_REMOVED && cur_qss_status != QSS_STATUS_SIM_REMOVED) ||
        (prev_qss_status > QSS_STATUS_SIM_REMOVED && cur_qss_status == QSS_STATUS_SIM_REMOVED) ||
        (cur_qss_active_slot != prev_qss_active_slot)) {
        mm_info ("QSS handler: SIM swap detected");
        mm_broadband_modem_update_sim_hot_swap_detected (MM_BROADBAND_MODEM (self));
    }
}

static void
qss_setup_context_free (QssSetupContext *ctx)
{
    g_clear_object (&(ctx->primary));
    g_clear_object (&(ctx->secondary));
    g_clear_error (&(ctx->primary_error));
    g_clear_error (&(ctx->secondary_error));
    g_slice_free (QssSetupContext, ctx);
}

static gboolean
modem_setup_sim_hot_swap_finish (MMIfaceModem *self,
                                 GAsyncResult *res,
                                 GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
telit_qss_enable_ready (MMBaseModem *self,
                        GAsyncResult *res,
                        GTask *task)
{
    QssSetupContext *ctx;
    MMPortSerialAt *port;
    GError **error;
    GRegex *pattern;

    ctx = g_task_get_task_data (task);

    if (ctx->step == QSS_SETUP_STEP_ENABLE_PRIMARY_PORT) {
        port = ctx->primary;
        error = &ctx->primary_error;
    } else if (ctx->step == QSS_SETUP_STEP_ENABLE_SECONDARY_PORT) {
        port = ctx->secondary;
        error = &ctx->secondary_error;
    } else
        g_assert_not_reached ();

    if (!mm_base_modem_at_command_full_finish (self, res, error)) {
        mm_warn ("QSS: error enabling unsolicited on port %s: %s", mm_port_get_device (MM_PORT (port)), (*error)->message);
        goto next_step;
    }

    pattern = g_regex_new ("#QSS:\\s*([0-3])\\r\\n", G_REGEX_RAW, 0, NULL);
    g_assert (pattern);
    mm_port_serial_at_add_unsolicited_msg_handler (
        port,
        pattern,
        (MMPortSerialAtUnsolicitedMsgFn)telit_qss_unsolicited_handler,
        self,
        NULL);
    g_regex_unref (pattern);

    /* Some modems (e.g. LM940) have a second parameter in the #QSS msg that
     * corresponds to the active SIM slot. This always seems to be "0" when
     * using AT#SIMDET to change active SIM slot, but luckily changing between
     * two populated SIM slots still triggers removal and addition events. */
    pattern = g_regex_new ("#QSS:\\s*([0-3]),([0-1])\\r\\n", G_REGEX_RAW, 0, NULL);
    g_assert (pattern);
    mm_port_serial_at_add_unsolicited_msg_handler (
        port,
        pattern,
        (MMPortSerialAtUnsolicitedMsgFn)telit_qss_unsolicited_handler,
        self,
        NULL);
    g_regex_unref (pattern);

next_step:
    ctx->step++;
    qss_setup_step (task);
}

static void
telit_qss_query_ready (MMBaseModem *_self,
                       GAsyncResult *res,
                       GTask *task)
{
    MMBroadbandModemQmiTelit *self;
    GError *error = NULL;
    const gchar *response;
    MMTelitQssStatus qss_status;
    QssSetupContext *ctx;

    self = MM_BROADBAND_MODEM_QMI_TELIT (_self);
    ctx = g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (_self, res, &error);
    if (error) {
        mm_warn ("Could not get \"#QSS?\" reply: %s", error->message);
        g_error_free (error);
        goto next_step;
    }

    qss_status = mm_telit_parse_qss_query (response, &error);
    if (error) {
        mm_warn ("QSS query parse error: %s", error->message);
        g_error_free (error);
        goto next_step;
    }

    mm_info ("QSS: current status is '%s'", mm_telit_qss_status_get_string (qss_status));
    self->priv->qss_status = qss_status;

next_step:
    ctx->step++;
    qss_setup_step (task);
}

static void
qss_setup_step (GTask *task)
{
    QssSetupContext *ctx;
    MMBroadbandModemQmiTelit *self;

    self = MM_BROADBAND_MODEM_QMI_TELIT (g_task_get_source_object (task));
    ctx = g_task_get_task_data (task);

    switch (ctx->step) {
        case QSS_SETUP_STEP_FIRST:
            /* Fall back on next step */
            ctx->step++;
        case QSS_SETUP_STEP_QUERY:
            mm_base_modem_at_command (MM_BASE_MODEM (self),
                                      "#QSS?",
                                      3,
                                      FALSE,
                                      (GAsyncReadyCallback) telit_qss_query_ready,
                                      task);
            return;
        case QSS_SETUP_STEP_ENABLE_PRIMARY_PORT:
            mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                           ctx->primary,
                                           "#QSS=1",
                                           3,
                                           FALSE,
                                           FALSE, /* raw */
                                           NULL, /* cancellable */
                                           (GAsyncReadyCallback) telit_qss_enable_ready,
                                           task);
            return;
        case QSS_SETUP_STEP_ENABLE_SECONDARY_PORT:
            if (ctx->secondary) {
                mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                               ctx->secondary,
                                               "#QSS=1",
                                               3,
                                               FALSE,
                                               FALSE, /* raw */
                                               NULL, /* cancellable */
                                               (GAsyncReadyCallback) telit_qss_enable_ready,
                                               task);
                return;
            }
            /* Fall back to next step */
            ctx->step++;
        case QSS_SETUP_STEP_LAST:
            /* If all enabling actions failed (either both, or only primary if
             * there is no secondary), then we return an error */
            if (ctx->primary_error &&
                (ctx->secondary_error || !ctx->secondary))
                g_task_return_new_error (task,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_FAILED,
                                         "QSS: couldn't enable unsolicited");
            else
                g_task_return_boolean (task, TRUE);
            g_object_unref (task);
            break;
    }
}

static void
modem_setup_sim_hot_swap (MMIfaceModem *self,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
    QssSetupContext *ctx;
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    ctx = g_slice_new0 (QssSetupContext);
    ctx->step = QSS_SETUP_STEP_FIRST;
    ctx->primary = mm_base_modem_get_port_primary (MM_BASE_MODEM (self));
    ctx->secondary = mm_base_modem_get_port_secondary (MM_BASE_MODEM (self));

    g_task_set_task_data (task, ctx, (GDestroyNotify) qss_setup_context_free);
    qss_setup_step (task);
}

/*****************************************************************************/

MMBroadbandModemQmiTelit *
mm_broadband_modem_qmi_telit_new (const gchar *device,
                             const gchar **drivers,
                             const gchar *plugin,
                             guint16 vendor_id,
                             guint16 product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_QMI_TELIT,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_DRIVERS, drivers,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         MM_IFACE_MODEM_SIM_HOT_SWAP_SUPPORTED, TRUE,
                         MM_IFACE_MODEM_SIM_HOT_SWAP_CONFIGURED, FALSE,
                         NULL);
}

static void
mm_broadband_modem_qmi_telit_init (MMBroadbandModemQmiTelit *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_BROADBAND_MODEM_QMI_TELIT,
                                              MMBroadbandModemQmiTelitPrivate);

    self->priv->qss_status = QSS_STATUS_UNKNOWN;
    self->priv->qss_active_slot = 0;
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface_modem_parent = g_type_interface_peek_parent (iface);

    iface->setup_sim_hot_swap = modem_setup_sim_hot_swap;
    iface->setup_sim_hot_swap_finish = modem_setup_sim_hot_swap_finish;
}

static void
mm_broadband_modem_qmi_telit_class_init (MMBroadbandModemQmiTelitClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemQmiTelitPrivate));
}

