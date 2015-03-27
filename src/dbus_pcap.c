/*
 * dbus_pcap.c - monitors a bus and dumps messages to a pcap file
 * Copyright � 2014 Lucas Hong Tran <hongtd2k@gmail.com>
 * Copyright � 2011-2012 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "dbusdump_config.h"

#include <string.h>
#include <pcap/pcap.h>

#include "dbus_svc_info.h"
#include "dbus_pcap.h"

#ifdef G_LOG_DOMAIN
#undef G_LOG_DOMAIN
#endif

#define G_LOG_DOMAIN "DBUS_PCAP"

typedef struct {
    struct timeval ts;
    GByteArray *blob;
} Message;

typedef struct {
    DbusPcapMonitor *self;
    GDBusMessage *dbus_message;
    gboolean is_incoming;
    Message message;
} IdleEmitData;

#define STOP ((Message *) 0x1)

typedef struct {
    pcap_dumper_t *dumper;
    pcap_dumper_t *dumper_stdout;
    GAsyncQueue *message_queue;
} ThreadData;

struct _DbusPcapMonitorPrivate {
    GBusType bus_type;
    GDBusConnection *connection;
    GDBusCapabilityFlags caps;
    DbusSvcInfo *dbus_svc_info;

    guint filter_id;

    gchar *filename;
    pcap_t *p;
    gboolean is_stdout_dump;

    gboolean is_inject_dbus_ext_hdr;


    GThread *thread;
    ThreadData td;
};

enum {
    PROP_BUS_TYPE = 1,
    PROP_FILENAME,

    PROP_IS_INJECT_DBUS_EXT_HDR,

    PROP_IS_DUMP_STDOUT

};

enum {
    SIG_MESSAGE_LOGGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static GLogFunc       g_log_handler = NULL;

static void initable_iface_init (
    gpointer g_class,
    gpointer unused);

G_DEFINE_TYPE_WITH_CODE (DbusPcapMonitor, dbus_pcap_monitor, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init);
    )

static void
dbus_pcap_monitor_init (DbusPcapMonitor *self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, DBUS_PCAP_MONITOR_TYPE,
      DbusPcapMonitorPrivate);
  self->priv->bus_type = G_BUS_TYPE_SESSION;
  self->priv->td.message_queue = g_async_queue_new ();
}

static void
dbus_pcap_monitor_get_property (
    GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  DbusPcapMonitor *self = DBUS_PCAP_MONITOR (object);
  DbusPcapMonitorPrivate *priv = self->priv;

  switch (property_id)
    {
      case PROP_BUS_TYPE:
        g_value_set_enum (value, priv->bus_type);
        break;
      case PROP_FILENAME:
        g_value_set_string (value, priv->filename);
        break;

      case PROP_IS_DUMP_STDOUT:
        g_value_set_boolean(value, priv->is_stdout_dump);
        break;


      case PROP_IS_INJECT_DBUS_EXT_HDR:
        g_value_set_boolean(value, priv->is_inject_dbus_ext_hdr);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
dbus_pcap_monitor_set_property (
    GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  DbusPcapMonitor *self = DBUS_PCAP_MONITOR (object);
  DbusPcapMonitorPrivate *priv = self->priv;

  switch (property_id)
    {
      case PROP_BUS_TYPE:
        priv->bus_type = g_value_get_enum (value);
        break;
      case PROP_FILENAME:
        priv->filename = g_value_dup_string (value);
        break;

      case PROP_IS_DUMP_STDOUT:
        priv->is_stdout_dump = g_value_get_boolean(value);
        break;

      case PROP_IS_INJECT_DBUS_EXT_HDR:
        priv->is_inject_dbus_ext_hdr = g_value_get_boolean(value);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
dbus_pcap_monitor_dispose (GObject *object)
{
  DbusPcapMonitor *self = DBUS_PCAP_MONITOR (object);
  DbusPcapMonitorPrivate *priv = self->priv;
  GObjectClass *parent_class = dbus_pcap_monitor_parent_class;

  if (parent_class->dispose != NULL)
    parent_class->dispose (object);

  /* unref/dispose dbus_svc_info object */
  g_object_unref(priv->dbus_svc_info);
  priv->dbus_svc_info = NULL;

  /* Make sure we're all closed up. */
  dbus_pcap_monitor_stop (self);

  g_clear_object (&priv->connection);
}

static void
dbus_pcap_monitor_finalize (GObject *object)
{
  DbusPcapMonitor *self = DBUS_PCAP_MONITOR (object);
  DbusPcapMonitorPrivate *priv = self->priv;
  GObjectClass *parent_class = dbus_pcap_monitor_parent_class;

  if (parent_class->finalize != NULL)
    parent_class->finalize (object);

  g_free (priv->filename);
  priv->filename = NULL;

  g_async_queue_unref (priv->td.message_queue);
  priv->td.message_queue = NULL;
}

static void
dbus_pcap_monitor_class_init (DbusPcapMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec *param_spec;


  object_class->get_property = dbus_pcap_monitor_get_property;
  object_class->set_property = dbus_pcap_monitor_set_property;
  object_class->dispose = dbus_pcap_monitor_dispose;
  object_class->finalize = dbus_pcap_monitor_finalize;

  g_type_class_add_private (klass, sizeof (DbusPcapMonitorPrivate));

#define THRICE(x) x, x, x

  param_spec = g_param_spec_enum (THRICE ("bus-type"),
      G_TYPE_BUS_TYPE, G_BUS_TYPE_SESSION,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_BUS_TYPE, param_spec);

  param_spec = g_param_spec_string (THRICE ("filename"), NULL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_FILENAME, param_spec);

  param_spec = g_param_spec_boolean("is_dump_stdout", "is-dump-stdout",
			   "Indicates whether the port is installed or not.",
							     FALSE,
							  G_PARAM_READWRITE);
  g_object_class_install_property (object_class, PROP_IS_DUMP_STDOUT, param_spec);


  param_spec = g_param_spec_boolean("is_inject_dbus_ext_hdr", "is-inject-dbus-ext-hdr",
                 "Indicates whether injection for dbus extension header is enabled or not.",
                                   FALSE,
                                G_PARAM_READWRITE);
    g_object_class_install_property (object_class, PROP_IS_INJECT_DBUS_EXT_HDR, param_spec);

  /**
   * DbusPcapMonitor::message-logged:
   * @self: the monitor.
   * @message: the #GDBusMessage object logged.
   * @is_incoming: if %TRUE, @message has come in from the bus daemon; if
   *  %FALSE, this is a message we're in the process of sending. Note that this
   *  can be %TRUE for messages our process sends, because we eavesdrop all
   *  messages, including our own.
   * @sec: seconds since 1970.
   * @usec: microseconds! (These are not combined into a single %gint64 because
   *  my version of gtk2hs crashes when it encounters %G_TYPE_UINT64 in a
   *  #GValue.)
   * @blob: an array of bytes containing the serialized message.
   * @length: the size in bytes of @blob.
   */
  signals[SIG_MESSAGE_LOGGED] = g_signal_new ("message-logged",
      DBUS_PCAP_MONITOR_TYPE, G_SIGNAL_RUN_FIRST,
      0, NULL, NULL,
      NULL, G_TYPE_NONE, 6,
      G_TYPE_DBUS_MESSAGE,
      G_TYPE_BOOLEAN,
      G_TYPE_LONG,
      G_TYPE_LONG,
      G_TYPE_POINTER,
      G_TYPE_UINT);
}

static gpointer
log_thread (gpointer data)
{
  ThreadData *td = data;
  Message *message;

  while (STOP != (message = g_async_queue_pop (td->message_queue)))
    {
      struct pcap_pkthdr hdr;
      hdr.ts = message->ts;

      hdr.caplen = message->blob->len;
      hdr.len = message->blob->len;
      /* The cast is necessary because libpcap is weird. */
      pcap_dump ((u_char *) td->dumper, &hdr, message->blob->data);
      pcap_dump_flush((u_char *) td->dumper);
      if (NULL != td->dumper_stdout )
      {
        pcap_dump((u_char *) td->dumper_stdout, &hdr, message->blob->data);
        pcap_dump_flush((u_char *) td->dumper_stdout);
      }

      g_byte_array_unref (message->blob);
      g_slice_free (Message, message);
    }

  g_printerr("\nSTOP!!!\n");

  return NULL;
}

static gboolean
emit_me (gpointer data)
{
  IdleEmitData *ied = data;
  DbusPcapMonitor *self = DBUS_PCAP_MONITOR (ied->self);
  glong sec = ied->message.ts.tv_sec;
  glong usec = ied->message.ts.tv_usec;

  g_signal_emit (self, signals[SIG_MESSAGE_LOGGED], 0,
      ied->dbus_message,
      ied->is_incoming,
      sec,
      usec,
      ied->message.blob->data,
      ied->message.blob->len);
  g_object_unref (self);
  g_object_unref (ied->dbus_message);
  g_byte_array_unref (ied->message.blob);
  g_slice_free (IdleEmitData, ied);
  return FALSE;
}

GDBusMessage *
filter (
    GDBusConnection *connection,
    GDBusMessage *locked_message,
    gboolean is_incoming,
    gpointer user_data)
{
  DbusPcapMonitor *self = DBUS_PCAP_MONITOR (user_data);
  DbusPcapMonitorPrivate *priv= self->priv;
  const gchar *dest;
  gsize size;
  guchar *blob;
  IdleEmitData ied = { g_object_ref (self), NULL, is_incoming };
  GError *error = NULL;
  GDBusMessage *message;

  gboolean is_name_owner_changed_signal = FALSE;


  message = g_dbus_message_copy(locked_message, &error);
  ied.dbus_message = g_object_ref (message);

  /* Marshalling message to byte array send send to async queue */
  gettimeofday (&ied.message.ts, NULL);


  is_name_owner_changed_signal = (
          G_DBUS_MESSAGE_TYPE_SIGNAL == g_dbus_message_get_message_type(message)) &&
          (0== g_strcmp0(g_dbus_message_get_member(message), "NameOwnerChanged"));
g_debug("priv->is_inject_dbus_ext_hdr = %d", priv->is_inject_dbus_ext_hdr);
  if ((priv->is_inject_dbus_ext_hdr) && (!is_name_owner_changed_signal))
  {
      /* Inject special header field for pid and path of src/dst */
      g_debug("Inject special header field for pid and path of src/dst");
      dbus_svc_info_inject_pid_path(priv->dbus_svc_info, message, &error);
  }


  blob = g_dbus_message_to_blob (message, &size, priv->caps, &error);
  if (blob == NULL)
    {
      g_critical ("Couldn't marshal message: %s", error->message);
      g_return_val_if_reached (NULL);
    }
  if (size > G_MAXUINT)
    {
      g_critical ("Message is longer than " G_STRINGIFY (G_MAXUINT)
                  "(which is surprising because the specification says the "
                  "maximum length of a message is 2**27 and guint is always "
                  "at least 32 bits wide");
      g_return_val_if_reached (NULL);
    }
  ied.message.blob = g_byte_array_append (
      g_byte_array_sized_new ((guint) size),
      blob, (guint) size);

  g_byte_array_ref (ied.message.blob);
  g_async_queue_push (priv->td.message_queue,
      g_slice_dup (Message, &(ied.message)));

  dest = g_dbus_message_get_destination (message);

  /* The idle steals the remaining refs to self, message, and message_blob. */
  g_idle_add (emit_me, g_slice_dup (IdleEmitData, &ied));

  /* check for whether allow the escaped msg from this filter */
  if (!is_incoming ||
      g_strcmp0 (dest, g_dbus_connection_get_unique_name (connection)) == 0)
    {
      /* This message is either outgoing or actually for us, as opposed to
       * being eavesdropped; it should be allowed to escape from this handler.
       */
      g_debug("filter escaped msg with dst=%s", dest);
      return locked_message;
    }


  else if (is_name_owner_changed_signal)
  {
      /* Signal NameOwnerChanged*/
      g_debug("filter escaped msg NameOwnerChanged");
      return locked_message;
  }

  else
  {
      /* Otherwise, we need to handle it within this function, or else GDBus
       * replies to other people's method calls and we all get really confused.
       */
      g_clear_object (&locked_message);
      return NULL;
  }

  return NULL;
}

static gboolean
match_everything (
    GDBusProxy *bus,
    gboolean with_eavesdrop,
    GError **error)
{
#define EAVESDROP "eavesdrop=true,"
  char *rules[] = {
      EAVESDROP "type='signal'",
      EAVESDROP "type='method_call'",
      EAVESDROP "type='method_return'",
      EAVESDROP "type='error'",
      NULL
  };
  const gsize offset = with_eavesdrop ? 0 : strlen (EAVESDROP);
  char **r;

  for (r = rules; *r != NULL; r++)
    {
      const gchar *rule = *r + offset;
      GVariant *ret = g_dbus_proxy_call_sync (
          bus,
          "AddMatch",
          g_variant_new ("(s)", rule),
          G_DBUS_CALL_FLAGS_NONE,
          -1,
          NULL,
          error);

      if (ret == NULL)
        {
          g_prefix_error (error, "Couldn't AddMatch(%s): ", *r);
          return FALSE;
        }
      else
        {
          g_variant_unref (ret);
        }
    }

  return TRUE;
}

static gboolean
list_all_names (
    GDBusProxy *bus,
    GError **error)
{
  GVariant *ret;
  gchar **names;

  g_assert (G_IS_DBUS_PROXY (bus));

  ret = g_dbus_proxy_call_sync (bus, "ListNames", NULL,
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
  if (ret == NULL)
    {
      g_prefix_error (error, "Couldn't ListNames: ");
      return FALSE;
    }

  for (g_variant_get_child (ret, 0, "^a&s", &names);
       *names != NULL;
       names++)
    {
      gchar *name = *names;

      if (!g_dbus_is_unique_name (name) &&
          strcmp (name, "org.freedesktop.DBus") != 0)
        {
          GVariant *owner = g_dbus_proxy_call_sync (bus, "GetNameOwner",
              g_variant_new ("(s)", name),
              G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

          if (owner != NULL)
            g_variant_unref (owner);
          /* else they were too quick for us! */
        }
    }

  g_variant_unref (ret);
  return TRUE;
}

static gboolean
initable_init (
    GInitable *initable,
    GCancellable *cancellable,
    GError **error)
{
  DbusPcapMonitor *self = DBUS_PCAP_MONITOR (initable);
  DbusPcapMonitorPrivate *priv = self->priv;
  gchar *address;
  GDBusProxy *bus;
  gboolean ret;

  if (priv->bus_type == G_BUS_TYPE_NONE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
          "Logging things other than message busses is not supported");
      return FALSE;
    }

  if (priv->filename == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
          "You must specify a filename");
      return FALSE;
    }

  priv->p = pcap_open_dead (DLT_DBUS, 1 << 27);
  if (priv->p == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
          "pcap_open_dead failed. wtf");
      return FALSE;
    }
   priv->td.dumper = pcap_dump_open (priv->p, priv->filename);

   if (priv->is_stdout_dump)
   {
     priv->td.dumper_stdout = pcap_dump_fopen (priv->p, stdout);

    if (priv->td.dumper_stdout == NULL)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Couldn't open stdout for pcap dumper %s", pcap_geterr (priv->p));
        return FALSE;
      }
   }
   else
   {
     priv->td.dumper_stdout = NULL;
   }

  if (priv->td.dumper == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
          "Couldn't open target file %s", pcap_geterr (priv->p));
      return FALSE;
    }

  priv->thread = g_thread_create (log_thread, &priv->td, TRUE, error);
  if (priv->thread == NULL)
    {
      g_prefix_error (error, "Couldn't spawn logging thread: ");
      return FALSE;
    }

  address = g_dbus_address_get_for_bus_sync (priv->bus_type, NULL, error);
  if (address == NULL)
    {
      g_prefix_error (error, "Couldn't get %s bus address: ",
          priv->bus_type == G_BUS_TYPE_SESSION ? "session" : "system");
      return FALSE;
    }

  if (*address == '\0')
    {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_FAILED,
          "Failed to look up the %s bus address. %s",
          priv->bus_type == G_BUS_TYPE_SESSION ? "session" : "system",
          priv->bus_type == G_BUS_TYPE_SESSION
              ? "Is DBUS_SESSION_BUS_ADDRESS properly set?"
              : "");
      g_free (address);
      return FALSE;
    }

  priv->connection = g_dbus_connection_new_for_address_sync (address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
      G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, /* auth observer */
      NULL, /* cancellable */
      error);

  g_free (address);
  if (priv->connection == NULL)
    {
      g_prefix_error (error, "Couldn't connect to %s bus: ",
          priv->bus_type == G_BUS_TYPE_SESSION ? "session" : "system");
      g_error("Can't open DBus connection");
      return FALSE;
    }
  priv->caps = g_dbus_connection_get_capabilities (priv->connection);

  bus = g_dbus_proxy_new_sync (priv->connection,
      G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
      G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
      NULL,
      "org.freedesktop.DBus",
      "/org/freedesktop/DBus",
      "org.freedesktop.DBus",
      NULL,
      error);
  if (bus == NULL)
    {
      g_prefix_error (error, "Couldn't construct bus proxy: ");
      return FALSE;
    }

  /* initialize dbus svc info object */
  priv->dbus_svc_info = dbus_svc_info_new (g_object_ref(bus), g_log_handler, error);
  if (NULL == priv->dbus_svc_info)
  {
    g_prefix_error (error, "Can't create dbus svc info");
    g_clear_object (&bus);
    return FALSE;
  }

  /* As of DBus 1.5.something you have to specify eavesdrop=true to be sure of
   * getting everything. (Specifically, you don't get directed signals unless
   * you specify it.)
   *
   * So first we try to add match rules with "eavesdrop=true" on them. If that
   * fails, we try again without that; if that also fails, we return the second error.
   */
  if (!match_everything (bus, TRUE, NULL) &&
      !match_everything (bus, FALSE, error))
  {
      g_clear_object (&bus);
    return FALSE;
  }
  priv->filter_id = g_dbus_connection_add_filter (priv->connection, filter,
      g_object_ref (self), g_object_unref);

/* FIXME: there's a race between listing all the names and binding to all
 * signals (and hence getting NameOwnerChanged). Old bustle-dbus-monitor had
 * it too.
 */

  ret = list_all_names (bus, error);
  if (FALSE == ret)
  {
      g_clear_object (&bus);
      return FALSE;
  }

  return TRUE;

}

/* FIXME: make this async? */
void
dbus_pcap_monitor_stop (
    DbusPcapMonitor *self)
{
  DbusPcapMonitorPrivate *priv = self->priv;

  if (priv->filter_id != 0)
    {
      g_return_if_fail (priv->connection != NULL);
      g_dbus_connection_remove_filter (priv->connection, priv->filter_id);
      priv->filter_id = 0;
    }

  if (priv->connection != NULL &&
      !g_dbus_connection_is_closed (priv->connection))
    {
      g_dbus_connection_close_sync (priv->connection, NULL, NULL);
    }

  if (priv->thread != NULL)
    {
      g_return_if_fail (priv->td.message_queue != NULL);
      /* Wait for the writer thread to spit out all the messages, then close up. */
      g_async_queue_push (priv->td.message_queue, STOP);
      g_thread_join (priv->thread);
      priv->thread = NULL;
    }

  if (priv->td.dumper != NULL)
    {
      pcap_dump_close (priv->td.dumper);
      priv->td.dumper = NULL;
    }

  if (priv->td.dumper_stdout != NULL)
    {
      pcap_dump_close (priv->td.dumper_stdout);
      priv->td.dumper_stdout = NULL;
    }
  if (priv->p != NULL)
    {
      pcap_close (priv->p);
      priv->p = NULL;
    }

}

static void
initable_iface_init (
    gpointer g_class,
    gpointer unused)
{
  GInitableIface *iface = g_class;

  iface->init = initable_init;
}

DbusPcapMonitor *
dbus_pcap_monitor_new (
    GBusType bus_type,
    const gchar *filename,
    gboolean is_dump_stdout,

    gboolean is_inject_dbus_ext_hdr,
    GLogFunc log_handler,
    GError **error)
{

  g_log_handler = log_handler;
  g_log_set_handler(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG,  g_log_handler, NULL);

  return g_initable_new (
      DBUS_PCAP_MONITOR_TYPE, NULL, error,
      "bus-type", bus_type,
      "filename", filename,
      "is-dump-stdout", is_dump_stdout,
      "is-inject-dbus-ext-hdr", is_inject_dbus_ext_hdr,
      NULL);
}
