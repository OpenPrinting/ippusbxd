/* Copyright (C) 2014 Daniel Dressler and contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <net/if.h>

#include "dnssd.h"
#include "logging.h"
#include "options.h"

/*
 * 'dnssd_callback()' - Handle DNS-SD registration events generic.
 */

static void
dnssd_callback(AvahiEntryGroup      *g,		/* I - Service */
	       AvahiEntryGroupState state)	/* I - Registration state */
{
  switch (state) {
  case AVAHI_ENTRY_GROUP_ESTABLISHED :
    /* The entry group has been established successfully */
    NOTE("Service entry for the printer successfully established.");
    break;
  case AVAHI_ENTRY_GROUP_COLLISION :
    ERR("DNS-SD service name for this printer already exists");
    break;
  case AVAHI_ENTRY_GROUP_FAILURE :
    ERR("Entry group failure: %s\n",
	avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));
    g_options.terminate = 1;
    break;
  case AVAHI_ENTRY_GROUP_UNCOMMITED:
  case AVAHI_ENTRY_GROUP_REGISTERING:
  default:
    break;
  }
}

/*
 * 'dnssd_callback()' - Handle DNS-SD registration events ipp.
 */

static void
dnssd_callback_ipp(AvahiEntryGroup      *g,	/* I - Service */
	       AvahiEntryGroupState state,	/* I - Registration state */
	       void                 *context)	/* I - Printer */
{
  (void)context;

  if (g == NULL || (g_options.dnssd_data->ipp_ref != NULL &&
		    g_options.dnssd_data->ipp_ref != g))
    return;
  dnssd_callback(g, state);
}

/*
 * 'dnssd_client_cb()' - Client callback for Avahi.
 *
 * Called whenever the client or server state changes...
 */

static void
dnssd_client_cb(AvahiClient      *c,		/* I - Client */
		AvahiClientState state,		/* I - Current state */
		void             *userdata)	/* I - User data (unused) */
{
  (void)userdata;
  int error;			/* Error code, if any */

  if (!c)
    return;

  switch (state) {
  default :
    NOTE("Ignore Avahi state %d.", state);
    break;

  case AVAHI_CLIENT_CONNECTING:
    NOTE("Waiting for Avahi server.");
    break;

  case AVAHI_CLIENT_S_RUNNING:
    NOTE("Avahi server connection got available, registering printer.");
    dnssd_register(c);
    break;

  case AVAHI_CLIENT_S_REGISTERING:
  case AVAHI_CLIENT_S_COLLISION:
    NOTE("Dropping printer registration because of possible host name change.");
    if (g_options.dnssd_data->ipp_ref)
      avahi_entry_group_reset(g_options.dnssd_data->ipp_ref);
    break;

  case AVAHI_CLIENT_FAILURE:
    if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED) {
      NOTE("Avahi server disappeared, unregistering printer");
      dnssd_unregister();
      /* Renewing client */
      if (g_options.dnssd_data->DNSSDClient)
	avahi_client_free(g_options.dnssd_data->DNSSDClient);
      if ((g_options.dnssd_data->DNSSDClient =
	   avahi_client_new(avahi_threaded_poll_get
			    (g_options.dnssd_data->DNSSDMaster),
			    AVAHI_CLIENT_NO_FAIL,
			    dnssd_client_cb, NULL, &error)) == NULL) {
	ERR("Error: Unable to initialize DNS-SD client.");
	g_options.terminate = 1;
      }
    } else {
      ERR("Avahi server connection failure: %s",
	  avahi_strerror(avahi_client_errno(c)));
      g_options.terminate = 1;
    }
    break;

  }
}

int dnssd_init()
{
  int error; /* Error code, if any */

  g_options.dnssd_data = calloc(1, sizeof(dnssd_t));
  if (g_options.dnssd_data == NULL) {
    ERR("Unable to allocate memory for DNS-SD broadcast data.");
    goto fail;
  }
  g_options.dnssd_data->DNSSDMaster = NULL;
  g_options.dnssd_data->DNSSDClient = NULL;
  g_options.dnssd_data->ipp_ref = NULL;

  if ((g_options.dnssd_data->DNSSDMaster = avahi_threaded_poll_new()) == NULL) {
    ERR("Error: Unable to initialize DNS-SD.");
    goto fail;
  }

  if ((g_options.dnssd_data->DNSSDClient = avahi_client_new(
           avahi_threaded_poll_get(g_options.dnssd_data->DNSSDMaster),
           AVAHI_CLIENT_NO_FAIL, dnssd_client_cb, NULL, &error)) == NULL) {
    ERR("Error: Unable to initialize DNS-SD client.");
    goto fail;
  }

  avahi_threaded_poll_start(g_options.dnssd_data->DNSSDMaster);

  NOTE("DNS-SD initialized.");

  return 0;

 fail:
  dnssd_shutdown();

  return -1;
}

void dnssd_shutdown()
{
  if (g_options.dnssd_data->DNSSDMaster) {
    avahi_threaded_poll_stop(g_options.dnssd_data->DNSSDMaster);
    dnssd_unregister();
  }

  if (g_options.dnssd_data->DNSSDClient) {
    avahi_client_free(g_options.dnssd_data->DNSSDClient);
    g_options.dnssd_data->DNSSDClient = NULL;
  }

  if (g_options.dnssd_data->DNSSDMaster) {
    avahi_threaded_poll_free(g_options.dnssd_data->DNSSDMaster);
    g_options.dnssd_data->DNSSDMaster = NULL;
  }

  free(g_options.dnssd_data);
  system("killall esclusbxd");
  NOTE("DNS-SD shut down.");
}

int dnssd_register(AvahiClient *c)
{
  AvahiStringList *ipp_txt;             /* DNS-SD IPP TXT record */
  char            temp[256];            /* Subtype service string */
  char            dnssd_name[1024];
  char            *dev_id = NULL;
  const char      *make;                /* I - Manufacturer */
  const char      *model;               /* I - Model name */
  const char      *serial = NULL;
  const char      *cmd;
  const char      *urf = NULL;
  int             pwgraster = 0,
                  appleraster = 0,
                  pclm = 0,
                  pdf = 0,
                  jpeg = 0;
  char            has_duplex = 'U',
                  has_color = 'U',
                  has_copies = 'U';
  char            formats[1024];        /* I - Supported formats */
  char            *ptr;
  int             error;

 /*
  * Parse the device ID for MFG, MDL, and CMD
  */

  dev_id = strdup(g_options.device_id);
  if ((ptr = strcasestr(dev_id, "MFG:")) == NULL)
    if ((ptr = strcasestr(dev_id, "MANUFACTURER:")) == NULL) {
      ERR("No manufacturer info in device ID");
      free(dev_id);
      return -1;
    }
  make = strchr(ptr, ':') + 1;
  if ((ptr = strcasestr(dev_id, "MDL:")) == NULL)
    if ((ptr = strcasestr(dev_id, "MODEL:")) == NULL) {
      ERR("No model info in device ID");
      free(dev_id);
      return -1;
    }
  model = strchr(ptr, ':') + 1;
  if ((ptr = strcasestr(dev_id, "SN:")) == NULL)
    if ((ptr = strcasestr(dev_id, "SERN:")) == NULL) {
      if ((ptr = strcasestr(dev_id, "SERIALNUMBER:")) == NULL) {
	NOTE("No serial number info in device ID");
      }
    }
  if (ptr)
    serial = strchr(ptr, ':') + 1;
  if ((ptr = strcasestr(dev_id, "URF:")) == NULL)
    NOTE("No URF info in device ID");
  if (ptr)
    urf = strchr(ptr, ':') + 1;
  if ((ptr = strcasestr(dev_id, "CMD:")) == NULL)
    if ((ptr = strcasestr(dev_id, "COMMAND SET:")) == NULL) {
      ERR("No page description language info in device ID");
      free(dev_id);
      return -1;
    }
  cmd = strchr(ptr, ':') + 1;
  ptr = strchr(make, ';');
  if (ptr) *ptr = '\0';
  ptr = strchr(model, ';');
  if (ptr) *ptr = '\0';
  if (serial) {
    ptr = strchr(serial, ';');
    if (ptr) *ptr = '\0';
  }
  ptr = strchr(cmd, ';');
  if (ptr) *ptr = '\0';
  if (urf) {
    ptr = strchr(urf, ';');
    if (ptr) *ptr = '\0';
  }

  if ((ptr = strcasestr(cmd, "pwg")) != NULL &&
      (ptr = strcasestr(ptr, "raster")) != NULL)
    pwgraster = 1;
  if (((ptr = strcasestr(cmd, "apple")) != NULL &&
       (ptr = strcasestr(ptr, "raster")) != NULL) ||
      ((ptr = strcasestr(cmd, "urf")) != NULL) ||
      urf != NULL)
    appleraster = 1;
  if ((ptr = strcasestr(cmd, "pclm")) != NULL)
    pclm = 1;
  if ((ptr = strcasestr(cmd, "pdf")) != NULL)
    pdf = 1;
  if ((ptr = strcasestr(cmd, "jpeg")) != NULL ||
      (ptr = strcasestr(cmd, "jpg")) != NULL)
    jpeg = 1;
  snprintf(formats, sizeof(formats),"%s%s%s%s%s",
	   (pdf ? "application/pdf," : ""),
	   (pwgraster ? "image/pwg-raster," : ""),
	   (appleraster ? "image/urf," : ""),
	   (pclm ? "application/PCLm," : ""),
	   (jpeg ? "image/jpeg," : ""));
  formats[strlen(formats) - 1] = '\0';
  if (urf) {
    if ((ptr = strcasestr(urf, "DM")) != NULL &&
	*(ptr + 2) >= '1' && *(ptr + 2) <= '4')
      has_duplex = 'T';
    else
      has_duplex = 'F';
    if ((ptr = strcasestr(urf, "CP")) != NULL) {
      ptr += 2;
      if ((*ptr >= '2' && *ptr <= '9') ||
	  (*ptr == '1' && *(ptr + 1) >= '0' && *(ptr + 1) <= '9'))
	has_copies = 'T';
      else
	has_copies = 'F';
    } else
      has_copies = 'F';
    if ((ptr = strcasestr(urf, "RGB")) != NULL)
      has_color = 'T';
    else
      has_color = 'F';
  }

 /*
  * Additional printer properties
  */

  snprintf(temp, sizeof(temp), "http://localhost:%d/", g_options.real_port);
  if (serial)
    snprintf(dnssd_name, sizeof(dnssd_name), "%s [%s]", model, serial);
  else
    snprintf(dnssd_name, sizeof(dnssd_name), "%s", model);

 /*
  * Create the TXT record for printer ...
  */

  ipp_txt = NULL;
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "rp=ipp/print");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "ty=%s %s", make, model);
  if (strcasecmp(g_options.interface, "lo") == 0)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "adminurl=%s", temp);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "product=(%s)", model);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "pdl=%s", formats);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "Color=%c", has_color);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "Duplex=%c", has_duplex);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "Copies=%c", has_copies);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "usb_MFG=%s", make);
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "usb_MDL=%s", model);
  if (urf)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "URF=%s", urf);
  else if (appleraster)
    ipp_txt = avahi_string_list_add_printf(ipp_txt,
                                           "URF=CP1,IS1,MT1,RS300,SRGB24,W8");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "priority=60");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "txtvers=1");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "qtotal=1");
  free(dev_id);

 /*
  * Register _printer._tcp (LPD) with port 0 to reserve the service name...
  */

  NOTE("Registering printer %s on interface %s for DNS-SD broadcasting ...",
       dnssd_name, g_options.interface);

  if (g_options.dnssd_data->ipp_ref == NULL)
    g_options.dnssd_data->ipp_ref =
      avahi_entry_group_new((c ? c : g_options.dnssd_data->DNSSDClient),
			    dnssd_callback_ipp, NULL);

  if (g_options.dnssd_data->ipp_ref == NULL) {
    ERR("Could not establish Avahi entry group");
    avahi_string_list_free(ipp_txt);
    return -1;
  }

  error = avahi_entry_group_add_service_strlst(
      g_options.dnssd_data->ipp_ref,
      (g_options.interface ? (int)if_nametoindex(g_options.interface)
                           : AVAHI_IF_UNSPEC),
      AVAHI_PROTO_UNSPEC, 0, dnssd_name, "_printer._tcp", NULL, NULL, 0, NULL);
  if (error)
    ERR("Error registering %s as Unix printer (_printer._tcp): %d", dnssd_name,
	error);
  else
    NOTE("Registered %s as Unix printer (_printer._tcp).", dnssd_name);

 /*
  * Then register the _ipp._tcp (IPP)...
  */

  error = avahi_entry_group_add_service_strlst(
      g_options.dnssd_data->ipp_ref,
      (g_options.interface ? (int)if_nametoindex(g_options.interface)
                           : AVAHI_IF_UNSPEC),
      AVAHI_PROTO_UNSPEC, 0, dnssd_name, "_ipp._tcp", NULL, NULL,
      g_options.real_port, ipp_txt);

  if (error) {
    ERR("Error registering %s as IPP printer (_ipp._tcp): %d", dnssd_name,
	error);
  } else {
    NOTE("Registered %s as IPP printer (_ipp._tcp).", dnssd_name);
    error = avahi_entry_group_add_service_subtype(
        g_options.dnssd_data->ipp_ref,
        (g_options.interface ? (int)if_nametoindex(g_options.interface)
                             : AVAHI_IF_UNSPEC),
        AVAHI_PROTO_UNSPEC, 0, dnssd_name, "_ipp._tcp", NULL,
        (appleraster && !pwgraster ? "_universal._sub._ipp._tcp"
                                   : "_print._sub._ipp._tcp"));
    if (error)
      ERR("Error registering subtype for IPP printer %s (_print._sub._ipp._tcp "
          "or _universal._sub._ipp._tcp): %d",
          dnssd_name, error);
    else
      NOTE(
          "Registered subtype for IPP printer %s (_print._sub._ipp._tcp or "
          "_universal._sub._ipp._tcp).",
          dnssd_name);
  }

 /*
  * Finally _http.tcp (HTTP) for the web interface...
  */

  error = avahi_entry_group_add_service_strlst(
      g_options.dnssd_data->ipp_ref,
      (g_options.interface ? (int)if_nametoindex(g_options.interface)
                           : AVAHI_IF_UNSPEC),
      AVAHI_PROTO_UNSPEC, 0, dnssd_name, "_http._tcp", NULL, NULL,
      g_options.real_port, NULL);
  if (error) {
    ERR("Error registering web interface of %s (_http._tcp): %d", dnssd_name,
	error);
  } else {
    NOTE("Registered web interface of %s (_http._tcp).", dnssd_name);
    error = avahi_entry_group_add_service_subtype(
        g_options.dnssd_data->ipp_ref,
        (g_options.interface ? (int)if_nametoindex(g_options.interface)
                             : AVAHI_IF_UNSPEC),
        AVAHI_PROTO_UNSPEC, 0, dnssd_name, "_http._tcp", NULL,
        "_printer._sub._http._tcp");
    if (error)
      ERR("Error registering subtype for web interface of %s "
          "(_printer._sub._http._tcp): %d",
          dnssd_name, error);
    else
      NOTE(
          "Registered subtype for web interface of %s "
          "(_printer._sub._http._tcp).",
          dnssd_name);
  }

 /*
  * Commit it printer ...
  */

  avahi_entry_group_commit(g_options.dnssd_data->ipp_ref);

  avahi_string_list_free(ipp_txt);

  return 0;
}

void dnssd_unregister()
{
  if (g_options.dnssd_data->ipp_ref) {
    avahi_entry_group_free(g_options.dnssd_data->ipp_ref);
    g_options.dnssd_data->ipp_ref = NULL;
  }
}
