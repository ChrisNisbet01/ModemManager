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
 * Copyright (C) 2015 - Marco Bascetta <marco.bascetta@sadel.it>
 */

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-iface-modem.h"
#include "mm-iface-modem-voice.h"
#include "mm-call-list.h"
#include "mm-log.h"

#define SUPPORT_CHECKED_TAG           "voice-support-checked-tag"
#define SUPPORTED_TAG                 "voice-supported-tag"
#define CALL_LIST_POLLING_CONTEXT_TAG "voice-call-list-polling-context-tag"
#define IN_CALL_EVENT_CONTEXT_TAG     "voice-in-call-event-context-tag"

static GQuark support_checked_quark;
static GQuark supported_quark;
static GQuark call_list_polling_context_quark;
static GQuark in_call_event_context_quark;

/*****************************************************************************/

void
mm_iface_modem_voice_bind_simple_status (MMIfaceModemVoice *self,
                                         MMSimpleStatus *status)
{
}

/*****************************************************************************/

/* new calls will inherit audio settings if the modem is already in-call state */
static void update_audio_settings_in_call (MMIfaceModemVoice *self,
                                           MMBaseCall        *call);

static MMBaseCall *
create_incoming_call (MMIfaceModemVoice *self,
                      const gchar       *number)
{
    MMBaseCall *call;

    g_assert (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->create_call != NULL);

    call = MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->create_call (self, MM_CALL_DIRECTION_INCOMING, number);
    update_audio_settings_in_call (self, call);
    return call;
}

static MMBaseCall *
create_outgoing_call_from_properties (MMIfaceModemVoice  *self,
                                      MMCallProperties   *properties,
                                      GError            **error)
{
    MMBaseCall  *call;
    const gchar *number;

    /* Don't create CALL from properties if either number is missing */
    number = mm_call_properties_get_number (properties) ;
    if (!number) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_INVALID_ARGS,
                     "Cannot create call: mandatory parameter 'number' is missing");
        return NULL;
    }

    /* Create a call object as defined by the interface */
    g_assert (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->create_call != NULL);
    call = MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->create_call (self, MM_CALL_DIRECTION_OUTGOING, number);
    update_audio_settings_in_call (self, call);
    return call;
}

/*****************************************************************************/

void
mm_iface_modem_voice_report_incoming_call (MMIfaceModemVoice *self,
                                           const gchar       *number,
                                           MMCallState        state)
{
    MMBaseCall *call = NULL;
    MMCallList *list = NULL;

    g_assert (state == MM_CALL_STATE_RINGING_IN || state == MM_CALL_STATE_WAITING);

    g_object_get (MM_BASE_MODEM (self),
                  MM_IFACE_MODEM_VOICE_CALL_LIST, &list,
                  NULL);

    if (!list) {
        mm_warn ("Cannot create incoming call: missing call list");
        return;
    }

    call = mm_call_list_get_first_incoming_call (list, state);

    /* If call exists already, refresh its validity and set number if it wasn't set */
    if (call) {
        if (number && !mm_base_call_get_number (call))
            mm_base_call_set_number (call, number);
        mm_base_call_incoming_refresh (call);
        g_object_unref (list);
        return;
    }

    mm_dbg ("Creating new incoming call...");
    call = create_incoming_call (self, number);

    /* Set the state */
    mm_base_call_change_state (call, state, MM_CALL_STATE_REASON_INCOMING_NEW);

    /* Start its validity timeout */
    mm_base_call_incoming_refresh (call);

    /* Only export once properly created */
    mm_base_call_export (call);
    mm_call_list_add_call (list, call);
    g_object_unref (call);
    g_object_unref (list);
}

/*****************************************************************************/
/* Full current call list reporting
 *
 * This method receives as input a list with all the currently active calls,
 * including the specific state they're in.
 *
 * This method should:
 *  - Check whether we're reporting a new call (i.e. not in our internal call
 *    list yet). We'll create a new call object if so.
 *  - Check whether any of the known calls has changed state, and if so,
 *    update it.
 *  - Check whether any of the known calls is NOT given in the input list of
 *    call infos, which would mean the call is terminated.
 */

typedef struct {
    GList *call_info_list;
} ReportAllCallsForeachContext;

static void
report_all_calls_foreach (MMBaseCall                   *call,
                          ReportAllCallsForeachContext *ctx)
{
    GList            *l;
    MMCallState      state;
    MMCallDirection  direction;
    const gchar     *number;
    guint            idx;

    /* fully ignore already terminated calls */
    state = mm_base_call_get_state (call);
    if (state == MM_CALL_STATE_TERMINATED)
        return;

    /* try to look for a matching call by direction/number/index */
    direction = mm_base_call_get_direction (call);
    number    = mm_base_call_get_number    (call);
    idx       = mm_base_call_get_index     (call);
    for (l = ctx->call_info_list; l; l = g_list_next (l)) {
        MMCallInfo *call_info = (MMCallInfo *)(l->data);

        /* Early detect if a known incoming call that was created
         * from a plain CRING URC (i.e. without caller number)
         * needs to have the number provided. We match by state
         * as well as there may be different types of incoming
         * calls reported here (e.g. ringing, waiting).
         */
        if ((direction == MM_CALL_DIRECTION_INCOMING) &&
            (call_info->direction == direction) &&
            (call_info->state == state) &&
            (call_info->number && !number))
            mm_base_call_set_number (call, call_info->number);

        /* Early detect if a known incoming/outgoing call does
         * not have a known call index yet.
         */
        if ((call_info->direction == direction) &&
            (call_info->state == state) &&
            (call_info->index && !idx)) {
            mm_base_call_set_index (call, call_info->index);
            idx = call_info->index; /* so that we match next properly */
        }

        /* Exact match? note that if both numbers are NULL, it will
         * also match (e.g. if network doesn't report the caller number).
         */
        if ((call_info->direction == direction) &&
            (g_strcmp0 (call_info->number, number) == 0) &&
            (call_info->index == idx)) {
            /* Update state if it changed */
            if (state != call_info->state)
                mm_base_call_change_state (call, call_info->state, MM_CALL_STATE_REASON_UNKNOWN);
            /* refresh if incoming and new state is not terminated */
            if ((call_info->state != MM_CALL_STATE_TERMINATED) &&
                (direction == MM_CALL_DIRECTION_INCOMING)) {
                mm_base_call_incoming_refresh (call);
            }
            /* delete item from list and halt iteration right away */
            ctx->call_info_list = g_list_delete_link (ctx->call_info_list, l);
            return;
        }
    }

    /* not found in list! this call is now terminated */
    mm_base_call_change_state (call, MM_CALL_STATE_TERMINATED, MM_CALL_STATE_REASON_UNKNOWN);
}

void
mm_iface_modem_voice_report_all_calls (MMIfaceModemVoice *self,
                                       GList             *call_info_list)
{
    ReportAllCallsForeachContext  ctx = { 0 };
    MMCallList                   *list = NULL;
    GList                        *l;

    /* Early debugging of the full list of calls */
    mm_dbg ("Reported %u ongoing calls", g_list_length (call_info_list));
    for (l = call_info_list; l; l = g_list_next (l)) {
        MMCallInfo *call_info = (MMCallInfo *)(l->data);

        mm_dbg ("call at index %u: direction %s, state %s, number %s",
                call_info->index,
                mm_call_direction_get_string (call_info->direction),
                mm_call_state_get_string (call_info->state),
                call_info->number ? call_info->number : "n/a");
    }

    /* Retrieve list of known calls */
    g_object_get (MM_BASE_MODEM (self),
                  MM_IFACE_MODEM_VOICE_CALL_LIST, &list,
                  NULL);
    if (!list) {
        mm_warn ("Cannot report all calls: missing call list");
        return;
    }

    /* Iterate over all the calls already known to us.
     * Whenever a known call is updated, it will be removed from the call info list */
    ctx.call_info_list = g_list_copy (call_info_list);
    mm_call_list_foreach (list, (MMCallListForeachFunc)report_all_calls_foreach, &ctx);

    /* Once processed, the call info list will have all calls that were unknown to
     * us, i.e. the new calls to create. We really only expect new incoming calls, so
     * we'll warn if we get any outgoing call reported here. */
    for (l = ctx.call_info_list; l; l = g_list_next (l)) {
        MMCallInfo *call_info = (MMCallInfo *)(l->data);

        if (call_info->direction == MM_CALL_DIRECTION_OUTGOING) {
            mm_warn ("unexpected outgoing call to number '%s' reported in call list: state %s",
                     call_info->number ? call_info->number : "n/a",
                     mm_call_state_get_string (call_info->state));
            continue;
        }

        if (call_info->direction == MM_CALL_DIRECTION_INCOMING) {
            MMBaseCall *call;

            /* We only expect either RINGING-IN or WAITING states */
            if ((call_info->state != MM_CALL_STATE_RINGING_IN) &&
                (call_info->state != MM_CALL_STATE_WAITING)) {
                    mm_warn ("unexpected incoming call to number '%s' reported in call list: state %s",
                             call_info->number ? call_info->number : "n/a",
                             mm_call_state_get_string (call_info->state));
                    continue;
            }

            mm_dbg ("Creating new incoming call...");
            call = create_incoming_call (self, call_info->number);

            /* Set the state and the index */
            mm_base_call_change_state (call, call_info->state, MM_CALL_STATE_REASON_INCOMING_NEW);
            mm_base_call_set_index (call, call_info->index);

            /* Start its validity timeout */
            mm_base_call_incoming_refresh (call);

            /* Only export once properly created */
            mm_base_call_export (call);
            mm_call_list_add_call (list, call);
            g_object_unref (call);
            continue;
        }

        mm_warn ("unexpected call to number '%s' reported in call list: state %s, direction unknown",
                 call_info->number ? call_info->number : "n/a",
                 mm_call_state_get_string (call_info->state));
    }
    g_list_free (ctx.call_info_list);
    g_object_unref (list);
}

/*****************************************************************************/

typedef struct {
    MmGdbusModemVoice *skeleton;
    GDBusMethodInvocation *invocation;
    MMIfaceModemVoice *self;
    gchar *path;
} HandleDeleteContext;

static void
handle_delete_context_free (HandleDeleteContext *ctx)
{
    g_object_unref (ctx->skeleton);
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->self);
    g_free (ctx->path);
    g_free (ctx);
}

static void
handle_delete_auth_ready (MMBaseModem *self,
                          GAsyncResult *res,
                          HandleDeleteContext *ctx)
{
    MMModemState modem_state = MM_MODEM_STATE_UNKNOWN;
    MMCallList *list = NULL;
    GError *error = NULL;

    if (!mm_base_modem_authorize_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_delete_context_free (ctx);
        return;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_STATE, &modem_state,
                  NULL);

    if (modem_state < MM_MODEM_STATE_ENABLED) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot delete call: device not yet enabled");
        handle_delete_context_free (ctx);
        return;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_VOICE_CALL_LIST, &list,
                  NULL);
    if (!list) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot delete call: missing call list");
        handle_delete_context_free (ctx);
        return;
    }

    if (!mm_call_list_delete_call (list, ctx->path, &error))
        g_dbus_method_invocation_take_error (ctx->invocation, error);
    else
        mm_gdbus_modem_voice_complete_delete_call (ctx->skeleton, ctx->invocation);

    handle_delete_context_free (ctx);
    g_object_unref (list);
}

static gboolean
handle_delete (MmGdbusModemVoice *skeleton,
               GDBusMethodInvocation *invocation,
               const gchar *path,
               MMIfaceModemVoice *self)
{
    HandleDeleteContext *ctx;

    ctx = g_new (HandleDeleteContext, 1);
    ctx->skeleton = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self = g_object_ref (self);
    ctx->path = g_strdup (path);

    mm_base_modem_authorize (MM_BASE_MODEM (self),
                             invocation,
                             MM_AUTHORIZATION_VOICE,
                             (GAsyncReadyCallback)handle_delete_auth_ready,
                             ctx);
    return TRUE;
}

/*****************************************************************************/

typedef struct {
    MmGdbusModemVoice *skeleton;
    GDBusMethodInvocation *invocation;
    MMIfaceModemVoice *self;
    GVariant *dictionary;
} HandleCreateContext;

static void
handle_create_context_free (HandleCreateContext *ctx)
{
    g_object_unref (ctx->skeleton);
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->self);
    g_variant_unref (ctx->dictionary);
    g_free (ctx);
}

static void
handle_create_auth_ready (MMBaseModem *self,
                          GAsyncResult *res,
                          HandleCreateContext *ctx)
{
    MMModemState modem_state = MM_MODEM_STATE_UNKNOWN;
    MMCallList *list = NULL;
    GError *error = NULL;
    MMCallProperties *properties;
    MMBaseCall *call;

    if (!mm_base_modem_authorize_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_create_context_free (ctx);
        return;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_STATE, &modem_state,
                  NULL);

    if (modem_state < MM_MODEM_STATE_ENABLED) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot create CALL: device not yet enabled");
        handle_create_context_free (ctx);
        return;
    }

    /* Parse input properties */
    properties = mm_call_properties_new_from_dictionary (ctx->dictionary, &error);
    if (!properties) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_create_context_free (ctx);
        return;
    }

    call = create_outgoing_call_from_properties (MM_IFACE_MODEM_VOICE (self), properties, &error);
    if (!call) {
        g_object_unref (properties);
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_create_context_free (ctx);
        return;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_VOICE_CALL_LIST, &list,
                  NULL);
    if (!list) {
        g_object_unref (properties);
        g_object_unref (call);
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot create CALL: missing CALL list");
        handle_create_context_free (ctx);
        return;
    }

    /* Only export once properly created */
    mm_base_call_export (call);
    mm_call_list_add_call (list, call);

    /* Complete the DBus call */
    mm_gdbus_modem_voice_complete_create_call (ctx->skeleton,
                                               ctx->invocation,
                                               mm_base_call_get_path (call));
    g_object_unref (call);

    g_object_unref (properties);
    g_object_unref (list);

    handle_create_context_free (ctx);
}

static gboolean
handle_create (MmGdbusModemVoice *skeleton,
               GDBusMethodInvocation *invocation,
               GVariant *dictionary,
               MMIfaceModemVoice *self)
{
    HandleCreateContext *ctx;

    ctx = g_new (HandleCreateContext, 1);
    ctx->skeleton = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self = g_object_ref (self);
    ctx->dictionary = g_variant_ref (dictionary);

    mm_base_modem_authorize (MM_BASE_MODEM (self),
                             invocation,
                             MM_AUTHORIZATION_VOICE,
                             (GAsyncReadyCallback)handle_create_auth_ready,
                             ctx);
    return TRUE;
}

/*****************************************************************************/

static gboolean
handle_list (MmGdbusModemVoice *skeleton,
             GDBusMethodInvocation *invocation,
             MMIfaceModemVoice *self)
{
    GStrv paths;
    MMCallList *list = NULL;
    MMModemState modem_state;

    modem_state = MM_MODEM_STATE_UNKNOWN;
    g_object_get (self,
                  MM_IFACE_MODEM_STATE, &modem_state,
                  NULL);

    if (modem_state < MM_MODEM_STATE_ENABLED) {
        g_dbus_method_invocation_return_error (invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot list CALL messages: "
                                               "device not yet enabled");
        return TRUE;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_VOICE_CALL_LIST, &list,
                  NULL);
    if (!list) {
        g_dbus_method_invocation_return_error (invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot list CALL: missing CALL list");
        return TRUE;
    }

    paths = mm_call_list_get_paths (list);
    mm_gdbus_modem_voice_complete_list_calls (skeleton,
                                              invocation,
                                              (const gchar *const *)paths);
    g_strfreev (paths);
    g_object_unref (list);
    return TRUE;
}

/*****************************************************************************/

typedef struct {
    MmGdbusModemVoice     *skeleton;
    GDBusMethodInvocation *invocation;
    MMIfaceModemVoice     *self;
    GList                 *active_calls;
    MMBaseCall            *next_call;
} HandleHoldAndAcceptContext;

static void
handle_hold_and_accept_context_free (HandleHoldAndAcceptContext *ctx)
{
    g_list_free_full (ctx->active_calls, g_object_unref);
    g_clear_object (&ctx->next_call);
    g_object_unref (ctx->skeleton);
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->self);
    g_slice_free (HandleHoldAndAcceptContext, ctx);
}

static void
hold_and_accept_ready (MMIfaceModemVoice          *self,
                       GAsyncResult               *res,
                       HandleHoldAndAcceptContext *ctx)
{
    GError *error = NULL;
    GList  *l;

    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->hold_and_accept_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_hold_and_accept_context_free (ctx);
        return;
    }

    for (l = ctx->active_calls; l; l = g_list_next (l))
        mm_base_call_change_state (MM_BASE_CALL (l->data), MM_CALL_STATE_HELD, MM_CALL_STATE_REASON_UNKNOWN);
    if (ctx->next_call)
        mm_base_call_change_state (ctx->next_call, MM_CALL_STATE_ACTIVE, MM_CALL_STATE_REASON_ACCEPTED);

    mm_gdbus_modem_voice_complete_hold_and_accept (ctx->skeleton, ctx->invocation);
    handle_hold_and_accept_context_free (ctx);
}

static void
prepare_hold_and_accept_foreach (MMBaseCall                 *call,
                                 HandleHoldAndAcceptContext *ctx)
{
    switch (mm_base_call_get_state (call)) {
    case MM_CALL_STATE_ACTIVE:
        ctx->active_calls = g_list_append (ctx->active_calls, g_object_ref (call));
        break;
    case MM_CALL_STATE_WAITING:
        g_clear_object (&ctx->next_call);
        ctx->next_call = g_object_ref (call);
        break;
    case MM_CALL_STATE_HELD:
        if (!ctx->next_call)
            ctx->next_call = g_object_ref (call);
        break;
    default:
        break;
    }
}

static void
handle_hold_and_accept_auth_ready (MMBaseModem                *self,
                                   GAsyncResult               *res,
                                   HandleHoldAndAcceptContext *ctx)
{
    MMModemState  modem_state = MM_MODEM_STATE_UNKNOWN;
    GError       *error = NULL;
    MMCallList   *list = NULL;

    if (!mm_base_modem_authorize_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_hold_and_accept_context_free (ctx);
        return;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_STATE, &modem_state,
                  NULL);

    if (modem_state < MM_MODEM_STATE_ENABLED) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot hold and accept: device not yet enabled");
        handle_hold_and_accept_context_free (ctx);
        return;
    }

    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->hold_and_accept ||
        !MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->hold_and_accept_finish) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "Cannot hold and accept: unsupported");
        handle_hold_and_accept_context_free (ctx);
        return;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_VOICE_CALL_LIST, &list,
                  NULL);
    if (!list) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot hold and accept: missing call list");
        handle_hold_and_accept_context_free (ctx);
        return;
    }
    mm_call_list_foreach (list, (MMCallListForeachFunc)prepare_hold_and_accept_foreach, ctx);
    g_object_unref (list);

    MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->hold_and_accept (MM_IFACE_MODEM_VOICE (self),
                                                                (GAsyncReadyCallback)hold_and_accept_ready,
                                                                ctx);
}

static gboolean
handle_hold_and_accept (MmGdbusModemVoice     *skeleton,
                        GDBusMethodInvocation *invocation,
                        MMIfaceModemVoice     *self)
{
    HandleHoldAndAcceptContext *ctx;

    ctx = g_slice_new0 (HandleHoldAndAcceptContext);
    ctx->skeleton   = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self       = g_object_ref (self);

    mm_base_modem_authorize (MM_BASE_MODEM (self),
                             invocation,
                             MM_AUTHORIZATION_VOICE,
                             (GAsyncReadyCallback)handle_hold_and_accept_auth_ready,
                             ctx);
    return TRUE;
}

/*****************************************************************************/

typedef struct {
    MmGdbusModemVoice     *skeleton;
    GDBusMethodInvocation *invocation;
    MMIfaceModemVoice     *self;
    GList                 *active_calls;
    MMBaseCall            *next_call;
} HandleHangupAndAcceptContext;

static void
handle_hangup_and_accept_context_free (HandleHangupAndAcceptContext *ctx)
{
    g_list_free_full (ctx->active_calls, g_object_unref);
    g_clear_object (&ctx->next_call);
    g_object_unref (ctx->skeleton);
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->self);
    g_slice_free (HandleHangupAndAcceptContext, ctx);
}

static void
hangup_and_accept_ready (MMIfaceModemVoice            *self,
                         GAsyncResult                 *res,
                         HandleHangupAndAcceptContext *ctx)
{
    GError *error = NULL;
    GList  *l;

    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->hangup_and_accept_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_hangup_and_accept_context_free (ctx);
        return;
    }

    for (l = ctx->active_calls; l; l = g_list_next (l))
        mm_base_call_change_state (MM_BASE_CALL (l->data), MM_CALL_STATE_TERMINATED, MM_CALL_STATE_REASON_TERMINATED);
    if (ctx->next_call)
        mm_base_call_change_state (ctx->next_call, MM_CALL_STATE_ACTIVE, MM_CALL_STATE_REASON_ACCEPTED);

    mm_gdbus_modem_voice_complete_hangup_and_accept (ctx->skeleton, ctx->invocation);
    handle_hangup_and_accept_context_free (ctx);
}

static void
prepare_hangup_and_accept_foreach (MMBaseCall                   *call,
                                   HandleHangupAndAcceptContext *ctx)
{
    switch (mm_base_call_get_state (call)) {
    case MM_CALL_STATE_ACTIVE:
        ctx->active_calls = g_list_append (ctx->active_calls, g_object_ref (call));
        break;
    case MM_CALL_STATE_WAITING:
        g_clear_object (&ctx->next_call);
        ctx->next_call = g_object_ref (call);
        break;
    case MM_CALL_STATE_HELD:
        if (!ctx->next_call)
            ctx->next_call = g_object_ref (call);
        break;
    default:
        break;
    }
}

static void
handle_hangup_and_accept_auth_ready (MMBaseModem                  *self,
                                     GAsyncResult                 *res,
                                     HandleHangupAndAcceptContext *ctx)
{
    MMModemState  modem_state = MM_MODEM_STATE_UNKNOWN;
    GError       *error = NULL;
    MMCallList   *list = NULL;

    if (!mm_base_modem_authorize_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_hangup_and_accept_context_free (ctx);
        return;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_STATE, &modem_state,
                  NULL);

    if (modem_state < MM_MODEM_STATE_ENABLED) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot hangup and accept: device not yet enabled");
        handle_hangup_and_accept_context_free (ctx);
        return;
    }

    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->hangup_and_accept ||
        !MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->hangup_and_accept_finish) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "Cannot hangup and accept: unsupported");
        handle_hangup_and_accept_context_free (ctx);
        return;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_VOICE_CALL_LIST, &list,
                  NULL);
    if (!list) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot hangup and accept: missing call list");
        handle_hangup_and_accept_context_free (ctx);
        return;
    }
    mm_call_list_foreach (list, (MMCallListForeachFunc)prepare_hangup_and_accept_foreach, ctx);
    g_object_unref (list);

    MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->hangup_and_accept (MM_IFACE_MODEM_VOICE (self),
                                                                  (GAsyncReadyCallback)hangup_and_accept_ready,
                                                                  ctx);
}

static gboolean
handle_hangup_and_accept (MmGdbusModemVoice     *skeleton,
                          GDBusMethodInvocation *invocation,
                          MMIfaceModemVoice     *self)
{
    HandleHangupAndAcceptContext *ctx;

    ctx = g_slice_new0 (HandleHangupAndAcceptContext);
    ctx->skeleton   = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self       = g_object_ref (self);

    mm_base_modem_authorize (MM_BASE_MODEM (self),
                             invocation,
                             MM_AUTHORIZATION_VOICE,
                             (GAsyncReadyCallback)handle_hangup_and_accept_auth_ready,
                             ctx);
    return TRUE;
}

/*****************************************************************************/

typedef struct {
    MmGdbusModemVoice     *skeleton;
    GDBusMethodInvocation *invocation;
    MMIfaceModemVoice     *self;
    GList                 *calls;
} HandleHangupAllContext;

static void
handle_hangup_all_context_free (HandleHangupAllContext *ctx)
{
    g_list_free_full (ctx->calls, g_object_unref);
    g_object_unref (ctx->skeleton);
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->self);
    g_slice_free (HandleHangupAllContext, ctx);
}

static void
hangup_all_ready (MMIfaceModemVoice      *self,
                  GAsyncResult           *res,
                  HandleHangupAllContext *ctx)
{
    GError *error = NULL;
    GList  *l;

    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->hangup_all_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_hangup_all_context_free (ctx);
        return;
    }

    for (l = ctx->calls; l; l = g_list_next (l))
        mm_base_call_change_state (MM_BASE_CALL (l->data), MM_CALL_STATE_TERMINATED, MM_CALL_STATE_REASON_TERMINATED);

    mm_gdbus_modem_voice_complete_hangup_all (ctx->skeleton, ctx->invocation);
    handle_hangup_all_context_free (ctx);
}

static void
prepare_hangup_all_foreach (MMBaseCall             *call,
                            HandleHangupAllContext *ctx)
{
    /* The implementation of this operation will usually be done with +CHUP, and we
     * know that +CHUP is implemented in different ways by different manufacturers.
     *
     * The 3GPP TS27.007 spec for +CHUP states that the "Execution command causes
     * the TA to hangup the current call of the MT." This sentence leaves a bit of open
     * interpretation to the implementors, because a current call can be considered only
     * the active ones, or otherwise any call (active, held or waiting).
     *
     * And so, the u-blox TOBY-L4 takes one interpretation and "In case of multiple
     * calls, all active calls will be released, while waiting and held calls are not".
     *
     * And the Cinterion PLS-8 takes a different interpretation and cancels all calls,
     * including the waiting and held ones.
     *
     * In this logic, we're going to terminate exclusively the ACTIVE calls only, and we
     * will leave the possible termination of waiting/held calls to be reported via
     * call state updates, e.g. +CLCC polling or other plugin-specific method. In the
     * case of the Cinterion PLS-8, we'll detect the termination of the waiting and
     * held calls via ^SLCC URCs.
     */
    switch (mm_base_call_get_state (call)) {
    case MM_CALL_STATE_DIALING:
    case MM_CALL_STATE_RINGING_OUT:
    case MM_CALL_STATE_RINGING_IN:
    case MM_CALL_STATE_ACTIVE:
        ctx->calls = g_list_append (ctx->calls, g_object_ref (call));
        break;
    case MM_CALL_STATE_WAITING:
    case MM_CALL_STATE_HELD:
    default:
        break;
    }
}

static void
handle_hangup_all_auth_ready (MMBaseModem            *self,
                              GAsyncResult           *res,
                              HandleHangupAllContext *ctx)
{
    MMModemState  modem_state = MM_MODEM_STATE_UNKNOWN;
    GError       *error = NULL;
    MMCallList   *list = NULL;

    if (!mm_base_modem_authorize_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_hangup_all_context_free (ctx);
        return;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_STATE, &modem_state,
                  NULL);

    if (modem_state < MM_MODEM_STATE_ENABLED) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot hangup all: device not yet enabled");
        handle_hangup_all_context_free (ctx);
        return;
    }

    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->hangup_all ||
        !MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->hangup_all_finish) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "Cannot hangup all: unsupported");
        handle_hangup_all_context_free (ctx);
        return;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_VOICE_CALL_LIST, &list,
                  NULL);
    if (!list) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot hangup all: missing call list");
        handle_hangup_all_context_free (ctx);
        return;
    }
    mm_call_list_foreach (list, (MMCallListForeachFunc)prepare_hangup_all_foreach, ctx);
    g_object_unref (list);

    MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->hangup_all (MM_IFACE_MODEM_VOICE (self),
                                                           (GAsyncReadyCallback)hangup_all_ready,
                                                           ctx);
}

static gboolean
handle_hangup_all (MmGdbusModemVoice     *skeleton,
                   GDBusMethodInvocation *invocation,
                   MMIfaceModemVoice     *self)
{
    HandleHangupAllContext *ctx;

    ctx = g_slice_new0 (HandleHangupAllContext);
    ctx->skeleton   = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self       = g_object_ref (self);

    mm_base_modem_authorize (MM_BASE_MODEM (self),
                             invocation,
                             MM_AUTHORIZATION_VOICE,
                             (GAsyncReadyCallback)handle_hangup_all_auth_ready,
                             ctx);
    return TRUE;
}

/*****************************************************************************/

typedef struct {
    MmGdbusModemVoice     *skeleton;
    GDBusMethodInvocation *invocation;
    MMIfaceModemVoice     *self;
    GList                 *calls;
} HandleTransferContext;

static void
handle_transfer_context_free (HandleTransferContext *ctx)
{
    g_list_free_full (ctx->calls, g_object_unref);
    g_object_unref (ctx->skeleton);
    g_object_unref (ctx->invocation);
    g_object_unref (ctx->self);
    g_slice_free (HandleTransferContext, ctx);
}

static void
transfer_ready (MMIfaceModemVoice     *self,
                GAsyncResult          *res,
                HandleTransferContext *ctx)
{
    GError *error = NULL;
    GList  *l;

    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->transfer_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_transfer_context_free (ctx);
        return;
    }

    for (l = ctx->calls; l; l = g_list_next (l))
        mm_base_call_change_state (MM_BASE_CALL (l->data), MM_CALL_STATE_TERMINATED, MM_CALL_STATE_REASON_TRANSFERRED);

    mm_gdbus_modem_voice_complete_transfer (ctx->skeleton, ctx->invocation);
    handle_transfer_context_free (ctx);
}

static void
prepare_transfer_foreach (MMBaseCall            *call,
                          HandleTransferContext *ctx)
{
    switch (mm_base_call_get_state (call)) {
    case MM_CALL_STATE_ACTIVE:
    case MM_CALL_STATE_HELD:
        ctx->calls = g_list_append (ctx->calls, g_object_ref (call));
        break;
    default:
        break;
    }
}

static void
handle_transfer_auth_ready (MMBaseModem           *self,
                            GAsyncResult          *res,
                            HandleTransferContext *ctx)
{
    MMModemState  modem_state = MM_MODEM_STATE_UNKNOWN;
    GError       *error = NULL;
    MMCallList   *list = NULL;

    if (!mm_base_modem_authorize_finish (self, res, &error)) {
        g_dbus_method_invocation_take_error (ctx->invocation, error);
        handle_transfer_context_free (ctx);
        return;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_STATE, &modem_state,
                  NULL);

    if (modem_state < MM_MODEM_STATE_ENABLED) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot transfer: device not yet enabled");
        handle_transfer_context_free (ctx);
        return;
    }

    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->transfer ||
        !MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->transfer_finish) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_UNSUPPORTED,
                                               "Cannot transfer: unsupported");
        handle_transfer_context_free (ctx);
        return;
    }

    g_object_get (self,
                  MM_IFACE_MODEM_VOICE_CALL_LIST, &list,
                  NULL);
    if (!list) {
        g_dbus_method_invocation_return_error (ctx->invocation,
                                               MM_CORE_ERROR,
                                               MM_CORE_ERROR_WRONG_STATE,
                                               "Cannot transfer: missing call list");
        handle_transfer_context_free (ctx);
        return;
    }
    mm_call_list_foreach (list, (MMCallListForeachFunc)prepare_transfer_foreach, ctx);
    g_object_unref (list);

    MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->transfer (MM_IFACE_MODEM_VOICE (self),
                                                         (GAsyncReadyCallback)transfer_ready,
                                                         ctx);
}

static gboolean
handle_transfer (MmGdbusModemVoice     *skeleton,
                 GDBusMethodInvocation *invocation,
                 MMIfaceModemVoice     *self)
{
    HandleTransferContext *ctx;

    ctx = g_slice_new0 (HandleTransferContext);
    ctx->skeleton   = g_object_ref (skeleton);
    ctx->invocation = g_object_ref (invocation);
    ctx->self       = g_object_ref (self);

    mm_base_modem_authorize (MM_BASE_MODEM (self),
                             invocation,
                             MM_AUTHORIZATION_VOICE,
                             (GAsyncReadyCallback)handle_transfer_auth_ready,
                             ctx);
    return TRUE;
}

/*****************************************************************************/
/* In-call setup operation
 *
 * It will setup URC handlers for all in-call URCs, and also setup the audio
 * channel if the plugin requires to do so.
 */

typedef enum {
    IN_CALL_SETUP_STEP_FIRST,
    IN_CALL_SETUP_STEP_UNSOLICITED_EVENTS,
    IN_CALL_SETUP_STEP_AUDIO_CHANNEL,
    IN_CALL_SETUP_STEP_LAST,
} InCallSetupStep;

typedef struct {
    InCallSetupStep    step;
    MMPort            *audio_port;
    MMCallAudioFormat *audio_format;
} InCallSetupContext;

static void
in_call_setup_context_free (InCallSetupContext *ctx)
{
    g_clear_object (&ctx->audio_port);
    g_clear_object (&ctx->audio_format);
    g_slice_free (InCallSetupContext, ctx);
}

static gboolean
in_call_setup_finish (MMIfaceModemVoice  *self,
                      GAsyncResult       *res,
                      MMPort            **audio_port,   /* optional */
                      MMCallAudioFormat **audio_format, /* optional */
                      GError            **error)
{
    InCallSetupContext *ctx;

    if (!g_task_propagate_boolean (G_TASK (res), error))
        return FALSE;

    ctx = g_task_get_task_data (G_TASK (res));
    if (audio_port) {
        *audio_port = ctx->audio_port;
        ctx->audio_port = NULL;
    }
    if (audio_format) {
        *audio_format = ctx->audio_format;
        ctx->audio_format = NULL;
    }

    return TRUE;
}

static void in_call_setup_context_step (GTask *task);

static void
setup_in_call_audio_channel_ready (MMIfaceModemVoice *self,
                                   GAsyncResult      *res,
                                   GTask             *task)
{
    InCallSetupContext *ctx;
    GError             *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->setup_in_call_audio_channel_finish (self,
                                                                                        res,
                                                                                        &ctx->audio_port,
                                                                                        &ctx->audio_format,
                                                                                        &error)) {
        mm_warn ("Couldn't setup in-call audio channel: %s", error->message);
        g_clear_error (&error);
    }

    ctx->step++;
    in_call_setup_context_step (task);
}

static void
setup_in_call_unsolicited_events_ready (MMIfaceModemVoice *self,
                                        GAsyncResult      *res,
                                        GTask             *task)
{
    InCallSetupContext *ctx;
    GError             *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->setup_in_call_unsolicited_events_finish (self, res, &error)) {
        mm_warn ("Couldn't setup in-call unsolicited events: %s", error->message);
        g_clear_error (&error);
    }

    ctx->step++;
    in_call_setup_context_step (task);
}

static void
in_call_setup_context_step (GTask *task)
{
    MMIfaceModemVoice  *self;
    InCallSetupContext *ctx;

    if (g_task_return_error_if_cancelled (task))
        return;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    switch (ctx->step) {
    case IN_CALL_SETUP_STEP_FIRST:
        ctx->step++;
        /* fall-through */
    case IN_CALL_SETUP_STEP_UNSOLICITED_EVENTS:
        if (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->setup_in_call_unsolicited_events &&
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->setup_in_call_unsolicited_events_finish) {
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->setup_in_call_unsolicited_events (
                self,
                (GAsyncReadyCallback) setup_in_call_unsolicited_events_ready,
                task);
            break;
        }
        ctx->step++;
        /* fall-through */
    case IN_CALL_SETUP_STEP_AUDIO_CHANNEL:
        if (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->setup_in_call_audio_channel &&
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->setup_in_call_audio_channel_finish) {
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->setup_in_call_audio_channel (
                self,
                (GAsyncReadyCallback) setup_in_call_audio_channel_ready,
                task);
            break;
        }
        ctx->step++;
        /* fall-through */
    case IN_CALL_SETUP_STEP_LAST:
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
}

static void
in_call_setup (MMIfaceModemVoice   *self,
               GCancellable        *cancellable,
               GAsyncReadyCallback  callback,
               gpointer             user_data)
{
    GTask              *task;
    InCallSetupContext *ctx;

    ctx = g_slice_new0 (InCallSetupContext);
    ctx->step = IN_CALL_SETUP_STEP_FIRST;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify) in_call_setup_context_free);

    in_call_setup_context_step (task);
}

/*****************************************************************************/
/* In-call cleanup operation
 *
 * It will cleanup audio channel settings and remove all in-call URC handlers.
 */

typedef enum {
    IN_CALL_CLEANUP_STEP_FIRST,
    IN_CALL_CLEANUP_STEP_AUDIO_CHANNEL,
    IN_CALL_CLEANUP_STEP_UNSOLICITED_EVENTS,
    IN_CALL_CLEANUP_STEP_LAST,
} InCallCleanupStep;

typedef struct {
    InCallCleanupStep step;
} InCallCleanupContext;

static gboolean
in_call_cleanup_finish (MMIfaceModemVoice  *self,
                        GAsyncResult       *res,
                        GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void in_call_cleanup_context_step (GTask *task);

static void
cleanup_in_call_unsolicited_events_ready (MMIfaceModemVoice *self,
                                          GAsyncResult      *res,
                                          GTask             *task)
{
    InCallCleanupContext *ctx;
    GError             *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->cleanup_in_call_unsolicited_events_finish (self, res, &error)) {
        mm_warn ("Couldn't cleanup in-call unsolicited events: %s", error->message);
        g_clear_error (&error);
    }

    ctx->step++;
    in_call_cleanup_context_step (task);
}

static void
cleanup_in_call_audio_channel_ready (MMIfaceModemVoice *self,
                                     GAsyncResult      *res,
                                     GTask             *task)
{
    InCallCleanupContext *ctx;
    GError             *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->cleanup_in_call_audio_channel_finish (self, res, &error)) {
        mm_warn ("Couldn't cleanup in-call audio channel: %s", error->message);
        g_clear_error (&error);
    }

    ctx->step++;
    in_call_cleanup_context_step (task);
}

static void
in_call_cleanup_context_step (GTask *task)
{
    MMIfaceModemVoice    *self;
    InCallCleanupContext *ctx;

    if (g_task_return_error_if_cancelled (task))
        return;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    switch (ctx->step) {
    case IN_CALL_CLEANUP_STEP_FIRST:
        ctx->step++;
        /* fall-through */
    case IN_CALL_CLEANUP_STEP_AUDIO_CHANNEL:
        if (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->cleanup_in_call_audio_channel &&
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->cleanup_in_call_audio_channel_finish) {
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->cleanup_in_call_audio_channel (
                self,
                (GAsyncReadyCallback) cleanup_in_call_audio_channel_ready,
                task);
            break;
        }
        ctx->step++;
        /* fall-through */
    case IN_CALL_CLEANUP_STEP_UNSOLICITED_EVENTS:
        if (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->cleanup_in_call_unsolicited_events &&
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->cleanup_in_call_unsolicited_events_finish) {
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->cleanup_in_call_unsolicited_events (
                self,
                (GAsyncReadyCallback) cleanup_in_call_unsolicited_events_ready,
                task);
            break;
        }
        ctx->step++;
        /* fall-through */
    case IN_CALL_CLEANUP_STEP_LAST:
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
}

static void
in_call_cleanup (MMIfaceModemVoice   *self,
                 GCancellable        *cancellable,
                 GAsyncReadyCallback  callback,
                 gpointer             user_data)
{
    GTask                *task;
    InCallCleanupContext *ctx;

    ctx = g_new0 (InCallCleanupContext, 1);
    ctx->step = IN_CALL_CLEANUP_STEP_FIRST;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, g_free);

    in_call_cleanup_context_step (task);
}

/*****************************************************************************/
/* In-call event handling logic
 *
 * This procedure will run a in-call setup async function whenever we detect
 * that there is at least one call that is ongoing. This setup function will
 * try to setup in-call unsolicited events as well as any audio channel
 * requirements.
 *
 * The procedure will run a in-call cleanup async function whenever we detect
 * that there are no longer any ongoing calls. The cleanup function will
 * cleanup the audio channel and remove the in-call unsolicited event handlers.
 */

typedef struct {
    guint              check_id;
    GCancellable      *setup_cancellable;
    GCancellable      *cleanup_cancellable;
    gboolean           in_call_state;
    MMPort            *audio_port;
    MMCallAudioFormat *audio_format;
} InCallEventContext;

static void
in_call_event_context_free (InCallEventContext *ctx)
{
    if (ctx->check_id)
        g_source_remove (ctx->check_id);
    if (ctx->cleanup_cancellable) {
        g_cancellable_cancel (ctx->cleanup_cancellable);
        g_clear_object (&ctx->cleanup_cancellable);
    }
    if (ctx->setup_cancellable) {
        g_cancellable_cancel (ctx->setup_cancellable);
        g_clear_object (&ctx->setup_cancellable);
    }
    g_clear_object (&ctx->audio_port);
    g_clear_object (&ctx->audio_format);
    g_slice_free (InCallEventContext, ctx);
}

static InCallEventContext *
get_in_call_event_context (MMIfaceModemVoice *self)
{
    InCallEventContext *ctx;

    if (G_UNLIKELY (!in_call_event_context_quark))
        in_call_event_context_quark = g_quark_from_static_string (IN_CALL_EVENT_CONTEXT_TAG);

    ctx = g_object_get_qdata (G_OBJECT (self), in_call_event_context_quark);
    if (!ctx) {
        /* Create context and keep it as object data */
        ctx = g_slice_new0 (InCallEventContext);
        g_object_set_qdata_full (
            G_OBJECT (self),
            in_call_event_context_quark,
            ctx,
            (GDestroyNotify)in_call_event_context_free);
    }

    return ctx;
}

static void
call_list_foreach_audio_settings (MMBaseCall         *call,
                                  InCallEventContext *ctx)
{
    if (mm_base_call_get_state (call) != MM_CALL_STATE_TERMINATED)
        return;
    mm_base_call_change_audio_settings (call, ctx->audio_port, ctx->audio_format);
}

static void
update_audio_settings_in_ongoing_calls (MMIfaceModemVoice *self)
{
    MMCallList         *list = NULL;
    InCallEventContext *ctx;

    ctx = get_in_call_event_context (self);

    g_object_get (MM_BASE_MODEM (self),
                  MM_IFACE_MODEM_VOICE_CALL_LIST, &list,
                  NULL);
    if (!list) {
        mm_warn ("Cannot update audio settings in active calls: missing internal call list");
        return;
    }

    mm_call_list_foreach (list, (MMCallListForeachFunc) call_list_foreach_audio_settings, ctx);
    g_clear_object (&list);
}

static void
update_audio_settings_in_call (MMIfaceModemVoice *self,
                               MMBaseCall        *call)
{
    InCallEventContext *ctx;

    ctx = get_in_call_event_context (self);
    mm_base_call_change_audio_settings (call, ctx->audio_port, ctx->audio_format);
}

static void
call_list_foreach_count_in_call (MMBaseCall *call,
                                 gpointer    user_data)
{
    guint *n_calls_in_call = (guint *)user_data;

    switch (mm_base_call_get_state (call)) {
    case MM_CALL_STATE_DIALING:
    case MM_CALL_STATE_RINGING_OUT:
    case MM_CALL_STATE_HELD:
    case MM_CALL_STATE_ACTIVE:
        *n_calls_in_call = *n_calls_in_call + 1;
        break;
    case MM_CALL_STATE_RINGING_IN:
    case MM_CALL_STATE_WAITING:
        /* NOTE: ringing-in and waiting calls are NOT yet in-call, e.g. there must
         * be no audio settings enabled and we must not enable in-call URC handling
         * yet. */
    default:
        break;
    }
}

static void
in_call_cleanup_ready (MMIfaceModemVoice *self,
                       GAsyncResult      *res)
{
    GError             *error = NULL;
    InCallEventContext *ctx;

    ctx = get_in_call_event_context (self);

    if (!in_call_cleanup_finish (self, res, &error)) {
        /* ignore cancelled operations */
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && !g_error_matches (error, MM_CORE_ERROR, MM_CORE_ERROR_CANCELLED))
            mm_warn ("Cannot cleanup in-call modem state: %s", error->message);
        g_clear_error (&error);
    } else {
        mm_dbg ("modem is no longer in-call state");
        ctx->in_call_state = FALSE;
        g_clear_object (&ctx->audio_port);
        g_clear_object (&ctx->audio_format);
    }

    g_clear_object (&ctx->cleanup_cancellable);
}

static void
in_call_setup_ready (MMIfaceModemVoice *self,
                     GAsyncResult      *res)
{
    GError             *error = NULL;
    InCallEventContext *ctx;

    ctx = get_in_call_event_context (self);

    if (!in_call_setup_finish (self, res, &ctx->audio_port, &ctx->audio_format, &error)) {
        /* ignore cancelled operations */
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && !g_error_matches (error, MM_CORE_ERROR, MM_CORE_ERROR_CANCELLED))
            mm_warn ("Cannot setup in-call modem state: %s", error->message);
        g_clear_error (&error);
    } else {
        mm_dbg ("modem is now in-call state");
        ctx->in_call_state = TRUE;
        update_audio_settings_in_ongoing_calls (self);
    }

    g_clear_object (&ctx->setup_cancellable);
}

static gboolean
call_list_check_in_call_events (MMIfaceModemVoice *self)
{
    InCallEventContext *ctx;
    MMCallList         *list = NULL;
    guint               n_calls_in_call = 0;

    ctx = get_in_call_event_context (self);
    ctx->check_id = 0;

    g_object_get (MM_BASE_MODEM (self),
                  MM_IFACE_MODEM_VOICE_CALL_LIST, &list,
                  NULL);
    if (!list) {
        mm_warn ("Cannot update in-call state: missing internal call list");
        goto out;
    }

    mm_call_list_foreach (list, (MMCallListForeachFunc) call_list_foreach_count_in_call, &n_calls_in_call);

    /* Need to setup in-call events? */
    if (n_calls_in_call > 0 && !ctx->in_call_state) {
        /* if setup already ongoing, do nothing */
        if (ctx->setup_cancellable)
            goto out;

        /* cancel ongoing cleanup if any */
        if (ctx->cleanup_cancellable) {
            g_cancellable_cancel (ctx->cleanup_cancellable);
            g_clear_object (&ctx->cleanup_cancellable);
        }

        /* run setup */
        mm_dbg ("Setting up in-call state...");
        ctx->setup_cancellable = g_cancellable_new ();
        in_call_setup (self, ctx->setup_cancellable, (GAsyncReadyCallback) in_call_setup_ready, NULL);
        goto out;
    }

    /* Need to cleanup in-call events? */
    if (n_calls_in_call == 0 && ctx->in_call_state) {
        /* if cleanup already ongoing, do nothing */
        if (ctx->cleanup_cancellable)
            goto out;

        /* cancel ongoing setup if any */
        if (ctx->setup_cancellable) {
            g_cancellable_cancel (ctx->setup_cancellable);
            g_clear_object (&ctx->setup_cancellable);
        }

        /* run cleanup */
        mm_dbg ("Cleaning up in-call state...");
        ctx->cleanup_cancellable = g_cancellable_new ();
        in_call_cleanup (self, ctx->cleanup_cancellable, (GAsyncReadyCallback) in_call_cleanup_ready, NULL);
        goto out;
    }

 out:
    g_clear_object (&list);
    return G_SOURCE_REMOVE;
}

static void
call_state_changed (MMIfaceModemVoice *self)
{
    InCallEventContext *ctx;

    ctx = get_in_call_event_context (self);
    if (ctx->check_id)
        return;

    /* Process check for in-call events in an idle, so that we can combine
     * together in the same check multiple call state updates happening
     * at the same time for different calls (e.g. when swapping active/held
     * calls). */
    ctx->check_id = g_idle_add ((GSourceFunc)call_list_check_in_call_events, self);
}

static void
setup_in_call_event_handling (MMCallList        *call_list,
                              const gchar       *call_path_added,
                              MMIfaceModemVoice *self)
{
    MMBaseCall *call;

    call = mm_call_list_get_call (call_list, call_path_added);
    g_assert (call);

    g_signal_connect_swapped (call,
                              "state-changed",
                              G_CALLBACK (call_state_changed),
                              self);
}

/*****************************************************************************/
/* Call list polling logic
 *
 * The call list polling is exclusively used to detect detailed call state
 * updates while a call is being established. Therefore, if there is no call
 * being established (i.e. all terminated, unknown or active), then there is
 * no polling to do.
 *
 * Any time we add a new call to the list, we'll setup polling if it's not
 * already running, and the polling logic itself will decide when the polling
 * should stop.
 */

#define CALL_LIST_POLLING_TIMEOUT_SECS 2

typedef struct {
    guint    polling_id;
    gboolean polling_ongoing;
} CallListPollingContext;

static void
call_list_polling_context_free (CallListPollingContext *ctx)
{
    if (ctx->polling_id)
        g_source_remove (ctx->polling_id);
    g_slice_free (CallListPollingContext, ctx);
}

static CallListPollingContext *
get_call_list_polling_context (MMIfaceModemVoice *self)
{
    CallListPollingContext *ctx;

    if (G_UNLIKELY (!call_list_polling_context_quark))
        call_list_polling_context_quark =  (g_quark_from_static_string (
                                                 CALL_LIST_POLLING_CONTEXT_TAG));

    ctx = g_object_get_qdata (G_OBJECT (self), call_list_polling_context_quark);
    if (!ctx) {
        /* Create context and keep it as object data */
        ctx = g_slice_new0 (CallListPollingContext);

        g_object_set_qdata_full (
            G_OBJECT (self),
            call_list_polling_context_quark,
            ctx,
            (GDestroyNotify)call_list_polling_context_free);
    }

    return ctx;
}

static gboolean call_list_poll (MMIfaceModemVoice *self);

static void
load_call_list_ready (MMIfaceModemVoice *self,
                      GAsyncResult      *res)
{
    CallListPollingContext *ctx;
    GList                  *call_info_list = NULL;
    GError                 *error = NULL;

    ctx = get_call_list_polling_context (self);
    ctx->polling_ongoing = FALSE;

    g_assert (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->load_call_list_finish);
    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->load_call_list_finish (self, res, &call_info_list, &error)) {
        mm_warn ("couldn't load call list: %s", error->message);
        g_error_free (error);
    }

    /* Always report the list even if NULL (it would mean no ongoing calls) */
    mm_iface_modem_voice_report_all_calls (self, call_info_list);
    mm_3gpp_call_info_list_free (call_info_list);

    /* setup the polling again */
    g_assert (!ctx->polling_id);
    ctx->polling_id = g_timeout_add_seconds (CALL_LIST_POLLING_TIMEOUT_SECS,
                                             (GSourceFunc) call_list_poll,
                                             self);
}

static void
call_list_foreach_count_establishing (MMBaseCall *call,
                                      gpointer    user_data)
{
    guint *n_calls_establishing = (guint *)user_data;

    switch (mm_base_call_get_state (call)) {
    case MM_CALL_STATE_DIALING:
    case MM_CALL_STATE_RINGING_OUT:
    case MM_CALL_STATE_RINGING_IN:
    case MM_CALL_STATE_HELD:
    case MM_CALL_STATE_WAITING:
        *n_calls_establishing = *n_calls_establishing + 1;
        break;
    default:
        break;
    }
}

static gboolean
call_list_poll (MMIfaceModemVoice *self)
{
    CallListPollingContext *ctx;
    MMCallList             *list = NULL;
    guint                   n_calls_establishing = 0;

    ctx = get_call_list_polling_context (self);
    ctx->polling_id = 0;

    g_object_get (MM_BASE_MODEM (self),
                  MM_IFACE_MODEM_VOICE_CALL_LIST, &list,
                  NULL);

    if (!list) {
        mm_warn ("Cannot poll call list: missing internal call list");
        goto out;
    }

    mm_call_list_foreach (list, (MMCallListForeachFunc) call_list_foreach_count_establishing, &n_calls_establishing);

    /* If there is at least ONE call being established, we need the call list */
    if (n_calls_establishing > 0) {
        mm_dbg ("%u calls being established: call list polling required", n_calls_establishing);
        ctx->polling_ongoing = TRUE;
        g_assert (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->load_call_list);
        MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->load_call_list (self,
                                                                   (GAsyncReadyCallback)load_call_list_ready,
                                                                   NULL);
    } else
        mm_dbg ("no calls being established: call list polling stopped");

out:
    g_clear_object (&list);
    return G_SOURCE_REMOVE;
}

static void
setup_call_list_polling (MMCallList        *call_list,
                         const gchar       *call_path_added,
                         MMIfaceModemVoice *self)
{
    CallListPollingContext *ctx;

    ctx = get_call_list_polling_context (self);

    if (!ctx->polling_id && !ctx->polling_ongoing)
        ctx->polling_id = g_timeout_add_seconds (CALL_LIST_POLLING_TIMEOUT_SECS,
                                                 (GSourceFunc) call_list_poll,
                                                 self);
}

/*****************************************************************************/

static void
update_call_list (MmGdbusModemVoice *skeleton,
                  MMCallList        *list)
{
    gchar **paths;

    paths = mm_call_list_get_paths (list);
    mm_gdbus_modem_voice_set_calls (skeleton, (const gchar *const *)paths);
    g_strfreev (paths);

    g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (skeleton));
}

static void
call_added (MMCallList        *list,
            const gchar       *call_path,
            MmGdbusModemVoice *skeleton)
{
    mm_dbg ("Added call at '%s'", call_path);
    update_call_list (skeleton, list);
    mm_gdbus_modem_voice_emit_call_added (skeleton, call_path);
}

static void
call_deleted (MMCallList        *list,
              const gchar       *call_path,
              MmGdbusModemVoice *skeleton)
{
    mm_dbg ("Deleted call at '%s'", call_path);
    update_call_list (skeleton, list);
    mm_gdbus_modem_voice_emit_call_deleted (skeleton, call_path);
}

/*****************************************************************************/

typedef struct _DisablingContext DisablingContext;
static void interface_disabling_step (GTask *task);

typedef enum {
    DISABLING_STEP_FIRST,
    DISABLING_STEP_DISABLE_UNSOLICITED_EVENTS,
    DISABLING_STEP_CLEANUP_UNSOLICITED_EVENTS,
    DISABLING_STEP_LAST
} DisablingStep;

struct _DisablingContext {
    DisablingStep step;
    MmGdbusModemVoice *skeleton;
};

static void
disabling_context_free (DisablingContext *ctx)
{
    if (ctx->skeleton)
        g_object_unref (ctx->skeleton);
    g_free (ctx);
}

gboolean
mm_iface_modem_voice_disable_finish (MMIfaceModemVoice *self,
                                     GAsyncResult *res,
                                     GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
disable_unsolicited_events_ready (MMIfaceModemVoice *self,
                                  GAsyncResult *res,
                                  GTask *task)
{
    DisablingContext *ctx;
    GError *error = NULL;

    MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->disable_unsolicited_events_finish (self, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Go on to next step */
    ctx = g_task_get_task_data (task);
    ctx->step++;
    interface_disabling_step (task);
}

static void
cleanup_unsolicited_events_ready (MMIfaceModemVoice *self,
                                  GAsyncResult *res,
                                  GTask *task)
{
    DisablingContext *ctx;
    GError *error = NULL;

    MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->cleanup_unsolicited_events_finish (self, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Go on to next step */
    ctx = g_task_get_task_data (task);
    ctx->step++;
    interface_disabling_step (task);
}

static void
interface_disabling_step (GTask *task)
{
    MMIfaceModemVoice *self;
    DisablingContext *ctx;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    switch (ctx->step) {
    case DISABLING_STEP_FIRST:
        /* Fall down to next step */
        ctx->step++;

    case DISABLING_STEP_DISABLE_UNSOLICITED_EVENTS:
        /* Allow cleaning up unsolicited events */
        if (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->disable_unsolicited_events &&
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->disable_unsolicited_events_finish) {
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->disable_unsolicited_events (
                self,
                (GAsyncReadyCallback)disable_unsolicited_events_ready,
                task);
            return;
        }
        /* Fall down to next step */
        ctx->step++;

    case DISABLING_STEP_CLEANUP_UNSOLICITED_EVENTS:
        /* Allow cleaning up unsolicited events */
        if (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->cleanup_unsolicited_events &&
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->cleanup_unsolicited_events_finish) {
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->cleanup_unsolicited_events (
                self,
                (GAsyncReadyCallback)cleanup_unsolicited_events_ready,
                task);
            return;
        }
        /* Fall down to next step */
        ctx->step++;

    case DISABLING_STEP_LAST:
        /* Clear CALL list */
        g_object_set (self,
                      MM_IFACE_MODEM_VOICE_CALL_LIST, NULL,
                      NULL);

        /* We are done without errors! */
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    g_assert_not_reached ();
}

void
mm_iface_modem_voice_disable (MMIfaceModemVoice *self,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
    DisablingContext *ctx;
    GTask *task;

    ctx = g_new0 (DisablingContext, 1);
    ctx->step = DISABLING_STEP_FIRST;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify)disabling_context_free);

    g_object_get (self,
                  MM_IFACE_MODEM_VOICE_DBUS_SKELETON, &ctx->skeleton,
                  NULL);
    if (!ctx->skeleton) {
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "Couldn't get interface skeleton");
        g_object_unref (task);
        return;
    }

    interface_disabling_step (task);
}

/*****************************************************************************/

typedef struct _EnablingContext EnablingContext;
static void interface_enabling_step (GTask *task);

typedef enum {
    ENABLING_STEP_FIRST,
    ENABLING_STEP_SETUP_UNSOLICITED_EVENTS,
    ENABLING_STEP_ENABLE_UNSOLICITED_EVENTS,
    ENABLING_STEP_LAST
} EnablingStep;

struct _EnablingContext {
    EnablingStep step;
    MmGdbusModemVoice *skeleton;
    guint mem1_storage_index;
};

static void
enabling_context_free (EnablingContext *ctx)
{
    if (ctx->skeleton)
        g_object_unref (ctx->skeleton);
    g_free (ctx);
}

gboolean
mm_iface_modem_voice_enable_finish (MMIfaceModemVoice *self,
                                    GAsyncResult *res,
                                    GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
setup_unsolicited_events_ready (MMIfaceModemVoice *self,
                                GAsyncResult *res,
                                GTask *task)
{
    EnablingContext *ctx;
    GError *error = NULL;

    MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->setup_unsolicited_events_finish (self, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Go on to next step */
    ctx = g_task_get_task_data (task);
    ctx->step++;
    interface_enabling_step (task);
}

static void
enable_unsolicited_events_ready (MMIfaceModemVoice *self,
                                 GAsyncResult *res,
                                 GTask *task)
{
    EnablingContext *ctx;
    GError *error = NULL;

    /* Not critical! */
    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->enable_unsolicited_events_finish (self, res, &error)) {
        mm_dbg ("Couldn't enable unsolicited events: '%s'", error->message);
        g_error_free (error);
    }

    /* Go on with next step */
    ctx = g_task_get_task_data (task);
    ctx->step++;
    interface_enabling_step (task);
}

static void
interface_enabling_step (GTask *task)
{
    MMIfaceModemVoice *self;
    EnablingContext *ctx;

    /* Don't run new steps if we're cancelled */
    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    switch (ctx->step) {
    case ENABLING_STEP_FIRST: {
        MMCallList *list;

        list = mm_call_list_new (MM_BASE_MODEM (self));
        g_object_set (self,
                      MM_IFACE_MODEM_VOICE_CALL_LIST, list,
                      NULL);

        /* Connect to list's signals */
        g_signal_connect (list,
                          MM_CALL_ADDED,
                          G_CALLBACK (call_added),
                          ctx->skeleton);
        g_signal_connect (list,
                          MM_CALL_DELETED,
                          G_CALLBACK (call_deleted),
                          ctx->skeleton);

        /* Setup monitoring for in-call event handling */
        g_signal_connect (list,
                          MM_CALL_ADDED,
                          G_CALLBACK (setup_in_call_event_handling),
                          self);

        /* Unless we're told not to, setup call list polling logic */
        if (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->load_call_list &&
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->load_call_list_finish) {
            gboolean periodic_call_list_check_disabled = FALSE;

            g_object_get (self,
                          MM_IFACE_MODEM_VOICE_PERIODIC_CALL_LIST_CHECK_DISABLED, &periodic_call_list_check_disabled,
                          NULL);
            if (!periodic_call_list_check_disabled) {
                mm_dbg ("periodic call list polling will be used if supported");
                g_signal_connect (list,
                                  MM_CALL_ADDED,
                                  G_CALLBACK (setup_call_list_polling),
                                  self);
            }
        }

        g_object_unref (list);

        /* Fall down to next step */
        ctx->step++;
    }

    case ENABLING_STEP_SETUP_UNSOLICITED_EVENTS:
        /* Allow setting up unsolicited events */
        if (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->setup_unsolicited_events &&
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->setup_unsolicited_events_finish) {
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->setup_unsolicited_events (
                self,
                (GAsyncReadyCallback)setup_unsolicited_events_ready,
                task);
            return;
        }
        /* Fall down to next step */
        ctx->step++;

    case ENABLING_STEP_ENABLE_UNSOLICITED_EVENTS:
        /* Allow setting up unsolicited events */
        if (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->enable_unsolicited_events &&
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->enable_unsolicited_events_finish) {
            MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->enable_unsolicited_events (
                self,
                (GAsyncReadyCallback)enable_unsolicited_events_ready,
                task);
            return;
        }
        /* Fall down to next step */
        ctx->step++;

    case ENABLING_STEP_LAST:
        /* We are done without errors! */
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    g_assert_not_reached ();
}

void
mm_iface_modem_voice_enable (MMIfaceModemVoice *self,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
    EnablingContext *ctx;
    GTask *task;

    ctx = g_new0 (EnablingContext, 1);
    ctx->step = ENABLING_STEP_FIRST;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify)enabling_context_free);

    g_object_get (self,
                  MM_IFACE_MODEM_VOICE_DBUS_SKELETON, &ctx->skeleton,
                  NULL);
    if (!ctx->skeleton) {
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "Couldn't get interface skeleton");
        g_object_unref (task);
        return;
    }

    interface_enabling_step (task);
}

/*****************************************************************************/

typedef struct _InitializationContext InitializationContext;
static void interface_initialization_step (GTask *task);

typedef enum {
    INITIALIZATION_STEP_FIRST,
    INITIALIZATION_STEP_CHECK_SUPPORT,
    INITIALIZATION_STEP_FAIL_IF_UNSUPPORTED,
    INITIALIZATION_STEP_LAST
} InitializationStep;

struct _InitializationContext {
    MmGdbusModemVoice *skeleton;
    InitializationStep step;
};

static void
initialization_context_free (InitializationContext *ctx)
{
    g_object_unref (ctx->skeleton);
    g_free (ctx);
}

static void
check_support_ready (MMIfaceModemVoice *self,
                     GAsyncResult *res,
                     GTask *task)
{
    InitializationContext *ctx;
    GError *error = NULL;

    if (!MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->check_support_finish (self,
                                                                          res,
                                                                          &error)) {
        if (error) {
            /* This error shouldn't be treated as critical */
            mm_dbg ("Voice support check failed: '%s'", error->message);
            g_error_free (error);
        }
    } else {
        /* Voice is supported! */
        g_object_set_qdata (G_OBJECT (self),
                            supported_quark,
                            GUINT_TO_POINTER (TRUE));
    }

    /* Go on to next step */
    ctx = g_task_get_task_data (task);
    ctx->step++;
    interface_initialization_step (task);
}

static void
interface_initialization_step (GTask *task)
{
    MMIfaceModemVoice *self;
    InitializationContext *ctx;

    /* Don't run new steps if we're cancelled */
    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    switch (ctx->step) {
    case INITIALIZATION_STEP_FIRST:
        /* Setup quarks if we didn't do it before */
        if (G_UNLIKELY (!support_checked_quark))
            support_checked_quark = (g_quark_from_static_string (
                                         SUPPORT_CHECKED_TAG));
        if (G_UNLIKELY (!supported_quark))
            supported_quark = (g_quark_from_static_string (
                                   SUPPORTED_TAG));

        /* Fall down to next step */
        ctx->step++;

    case INITIALIZATION_STEP_CHECK_SUPPORT:
        if (!GPOINTER_TO_UINT (g_object_get_qdata (G_OBJECT (self),
                                                   support_checked_quark))) {
            /* Set the checked flag so that we don't run it again */
            g_object_set_qdata (G_OBJECT (self),
                                support_checked_quark,
                                GUINT_TO_POINTER (TRUE));
            /* Initially, assume we don't support it */
            g_object_set_qdata (G_OBJECT (self),
                                supported_quark,
                                GUINT_TO_POINTER (FALSE));

            if (MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->check_support &&
                MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->check_support_finish) {
                MM_IFACE_MODEM_VOICE_GET_INTERFACE (self)->check_support (
                    self,
                    (GAsyncReadyCallback)check_support_ready,
                    task);
                return;
            }

            /* If there is no implementation to check support, assume we DON'T
             * support it. */
        }
        /* Fall down to next step */
        ctx->step++;

    case INITIALIZATION_STEP_FAIL_IF_UNSUPPORTED:
        if (!GPOINTER_TO_UINT (g_object_get_qdata (G_OBJECT (self),
                                                   supported_quark))) {
            g_task_return_new_error (task,
                                     MM_CORE_ERROR,
                                     MM_CORE_ERROR_UNSUPPORTED,
                                     "Voice not supported");
            g_object_unref (task);
            return;
        }
        /* Fall down to next step */
        ctx->step++;

    case INITIALIZATION_STEP_LAST:
        /* We are done without errors! */

        /* Handle method invocations */
        g_signal_connect (ctx->skeleton,
                          "handle-create-call",
                          G_CALLBACK (handle_create),
                          self);
        g_signal_connect (ctx->skeleton,
                          "handle-delete-call",
                          G_CALLBACK (handle_delete),
                          self);
        g_signal_connect (ctx->skeleton,
                          "handle-list-calls",
                          G_CALLBACK (handle_list),
                          self);
        g_signal_connect (ctx->skeleton,
                          "handle-hangup-and-accept",
                          G_CALLBACK (handle_hangup_and_accept),
                          self);
        g_signal_connect (ctx->skeleton,
                          "handle-hold-and-accept",
                          G_CALLBACK (handle_hold_and_accept),
                          self);
        g_signal_connect (ctx->skeleton,
                          "handle-hangup-all",
                          G_CALLBACK (handle_hangup_all),
                          self);
        g_signal_connect (ctx->skeleton,
                          "handle-transfer",
                          G_CALLBACK (handle_transfer),
                          self);

        /* Finally, export the new interface */
        mm_gdbus_object_skeleton_set_modem_voice (MM_GDBUS_OBJECT_SKELETON (self),
                                                  MM_GDBUS_MODEM_VOICE (ctx->skeleton));

        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    g_assert_not_reached ();
}

gboolean
mm_iface_modem_voice_initialize_finish (MMIfaceModemVoice *self,
                                        GAsyncResult *res,
                                        GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

void
mm_iface_modem_voice_initialize (MMIfaceModemVoice *self,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
    InitializationContext *ctx;
    MmGdbusModemVoice *skeleton = NULL;
    GTask *task;

    /* Did we already create it? */
    g_object_get (self,
                  MM_IFACE_MODEM_VOICE_DBUS_SKELETON, &skeleton,
                  NULL);
    if (!skeleton) {
        skeleton = mm_gdbus_modem_voice_skeleton_new ();

        g_object_set (self,
                      MM_IFACE_MODEM_VOICE_DBUS_SKELETON, skeleton,
                      NULL);
    }

    /* Perform async initialization here */

    ctx = g_new0 (InitializationContext, 1);
    ctx->step = INITIALIZATION_STEP_FIRST;
    ctx->skeleton = skeleton;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify)initialization_context_free);

    interface_initialization_step (task);
}

void
mm_iface_modem_voice_shutdown (MMIfaceModemVoice *self)
{
    /* Unexport DBus interface and remove the skeleton */
    mm_gdbus_object_skeleton_set_modem_voice (MM_GDBUS_OBJECT_SKELETON (self), NULL);
    g_object_set (self,
                  MM_IFACE_MODEM_VOICE_DBUS_SKELETON, NULL,
                  NULL);
}

/*****************************************************************************/

static void
iface_modem_voice_init (gpointer g_iface)
{
    static gboolean initialized = FALSE;

    if (initialized)
        return;

    /* Properties */
    g_object_interface_install_property
        (g_iface,
         g_param_spec_object (MM_IFACE_MODEM_VOICE_DBUS_SKELETON,
                              "Voice DBus skeleton",
                              "DBus skeleton for the Voice interface",
                              MM_GDBUS_TYPE_MODEM_VOICE_SKELETON,
                              G_PARAM_READWRITE));

    g_object_interface_install_property
        (g_iface,
         g_param_spec_object (MM_IFACE_MODEM_VOICE_CALL_LIST,
                              "CALL list",
                              "List of CALL objects managed in the interface",
                              MM_TYPE_CALL_LIST,
                              G_PARAM_READWRITE));

    g_object_interface_install_property
        (g_iface,
         g_param_spec_boolean (MM_IFACE_MODEM_VOICE_PERIODIC_CALL_LIST_CHECK_DISABLED,
                               "Periodic call list checks disabled",
                               "Whether periodic call list check are disabled.",
                               FALSE,
                               G_PARAM_READWRITE));

    initialized = TRUE;
}

GType
mm_iface_modem_voice_get_type (void)
{
    static GType iface_modem_voice_type = 0;

    if (!G_UNLIKELY (iface_modem_voice_type)) {
        static const GTypeInfo info = {
            sizeof (MMIfaceModemVoice), /* class_size */
            iface_modem_voice_init,     /* base_init */
            NULL,                           /* base_finalize */
        };

        iface_modem_voice_type = g_type_register_static (G_TYPE_INTERFACE,
                                                         "MMIfaceModemVoice",
                                                         &info,
                                                         0);

        g_type_interface_add_prerequisite (iface_modem_voice_type, MM_TYPE_IFACE_MODEM);
    }

    return iface_modem_voice_type;
}
