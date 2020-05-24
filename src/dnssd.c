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
#include <string.h>
#include <unistd.h>
#include <net/if.h>

#include "dnssd.h"
#include "logging.h"
#include "options.h"
#include "capabilities.h"



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
 * 'dnssd_callback()' - Handle DNS-SD registration events uscan.
 */

static void
dnssd_callback_uscan(AvahiEntryGroup      *g,	/* I - Service */
	       AvahiEntryGroupState state,	/* I - Registration state */
	       void                 *context)	/* I - Printer */
{
  (void)context;

  if (g == NULL || (g_options.dnssd_data->uscan_ref != NULL &&
		    g_options.dnssd_data->uscan_ref != g))
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
    if (g_options.dnssd_data->uscan_ref)
      avahi_entry_group_reset(g_options.dnssd_data->uscan_ref);
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
  g_options.dnssd_data->uscan_ref = NULL;

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
  NOTE("DNS-SD shut down.");
}

void * dnssd_escl_register(void *data)
{
  AvahiClient *c = (AvahiClient *)data;
  AvahiStringList *uscan_txt;             /* DNS-SD USCAN TXT record */
  AvahiStringList *ipp_txt;             /* DNS-SD USCAN TXT record */
  ippScanner      *scanner = NULL;
  int             error;
  char            temp[256];            /* Subtype service string */
 
  ippPrinter *printer = (ippPrinter *)calloc(1, sizeof(ippPrinter));
  ipp_request(printer, g_options.real_port);
 /*
  * Register _printer._tcp (LPD) with port 0 to reserve the service name...
  */

  g_options.dnssd_data->dnssd_name = strdup(printer->ty);

  snprintf(temp, sizeof(temp), "http://localhost:%d/", g_options.real_port);
  NOTE("Registering printer %s on interface %s for DNS-SD broadcasting ...",
       g_options.dnssd_data->dnssd_name, g_options.interface);
  //ipp_txt = (AvahiStringList *)data;
  ipp_txt = NULL;
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "rp=ipp/print");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "priority=60");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "txtvers=1");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "qtotal=1");

  if (printer->adminurl)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "adminurl=%s", printer->adminurl);
  else
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "adminurl=%s", temp);
  if (printer->uuid)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "UUID=%s", printer->uuid);
  if (printer->mopria_certified)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "mopria-certified=%s", printer->mopria_certified);
  if (printer->kind)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "kind=%s", printer->kind);
  if (printer->color)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "Color=%s", printer->color);
  if (printer->note)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "note=%s", printer->note);
  else
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "note=");
  if (printer->ty) {
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "ty=%s", printer->ty);
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "product=(%s)", printer->ty);
  }
  if (printer->pdl)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "pdl=%s", printer->pdl);
  if (printer->urf)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "URF=%s", printer->urf);
  if (printer->papermax)
     ipp_txt = avahi_string_list_add_printf(ipp_txt, "PaperMax=%s", printer->papermax);
  if (printer->side)
     ipp_txt = avahi_string_list_add_printf(ipp_txt, "Duplex=%s", printer->side);
  if (printer->fax) {
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "Fax=%s", printer->fax);
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "rfo=ipp/faxout");
  }
  else
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "Fax=F");
  if (printer->mfg)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "usb_MFG=%s", printer->mfg);
  if (printer->mfg)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "usb_MDL=%s", printer->mdl);
  if (printer->cmd)
    ipp_txt = avahi_string_list_add_printf(ipp_txt, "usb_CMD=%s", printer->cmd);

  NOTE("Printer TXT[\n\tadminurl=%s\n\tUUID=%s\t\n]\n", printer->adminurl, printer->uuid);
  if (c)
     g_options.dnssd_data->DNSSDClient = c;
  if (g_options.dnssd_data->ipp_ref == NULL)
    g_options.dnssd_data->ipp_ref =
      avahi_entry_group_new((c ? c : g_options.dnssd_data->DNSSDClient),
			    dnssd_callback_ipp, NULL);

  if (g_options.dnssd_data->ipp_ref == NULL) {
    ERR("Could not establish Avahi entry group");
    avahi_string_list_free(ipp_txt);
    return NULL;
  }

  error = avahi_entry_group_add_service_strlst(
      g_options.dnssd_data->ipp_ref,
      (g_options.interface ? (int)if_nametoindex(g_options.interface)
                           : AVAHI_IF_UNSPEC),
      AVAHI_PROTO_UNSPEC, 0, g_options.dnssd_data->dnssd_name, "_printer._tcp", NULL, NULL, 0, NULL);
  if (error)
    ERR("Error registering %s as Unix printer (_printer._tcp): %d", g_options.dnssd_data->dnssd_name,
	error);
  else
    NOTE("Registered %s as Unix printer (_printer._tcp).", g_options.dnssd_data->dnssd_name);

 /*
  * Then register the _ipp._tcp (IPP)...
  */

  error = avahi_entry_group_add_service_strlst(
      g_options.dnssd_data->ipp_ref,
      (g_options.interface ? (int)if_nametoindex(g_options.interface)
                           : AVAHI_IF_UNSPEC),
      AVAHI_PROTO_UNSPEC, 0, g_options.dnssd_data->dnssd_name, "_ipp._tcp", NULL, NULL,
      g_options.real_port, ipp_txt);

  if (error) {
    ERR("Error registering %s as IPP printer (_ipp._tcp): %d", g_options.dnssd_data->dnssd_name,
	error);
  } else {
    NOTE("Registered %s as IPP printer (_ipp._tcp).", g_options.dnssd_data->dnssd_name);
    error = avahi_entry_group_add_service_subtype(
        g_options.dnssd_data->ipp_ref,
        (g_options.interface ? (int)if_nametoindex(g_options.interface)
        : AVAHI_IF_UNSPEC),
        AVAHI_PROTO_UNSPEC, 0, g_options.dnssd_data->dnssd_name, "_ipp._tcp", NULL,
        "_print._sub._ipp._tcp");
    if (error)
      ERR("Error registering subtype for IPP printer %s (_print._sub._ipp._tcp "
          "or _universal._sub._ipp._tcp): %d",
          g_options.dnssd_data->dnssd_name, error);
    else
      NOTE(
          "Registered subtype for IPP printer %s (_print._sub._ipp._tcp or "
          "_universal._sub._ipp._tcp).",
          g_options.dnssd_data->dnssd_name);
  }

 /*
  * Finally _http.tcp (HTTP) for the web interface...
  */

  error = avahi_entry_group_add_service_strlst(
      g_options.dnssd_data->ipp_ref,
      (g_options.interface ? (int)if_nametoindex(g_options.interface)
                           : AVAHI_IF_UNSPEC),
      AVAHI_PROTO_UNSPEC, 0, g_options.dnssd_data->dnssd_name, "_http._tcp", NULL, NULL,
      g_options.real_port, NULL);
  if (error) {
    ERR("Error registering web interface of %s (_http._tcp): %d", g_options.dnssd_data->dnssd_name,
	error);
  } else {
    NOTE("Registered web interface of %s (_http._tcp).", g_options.dnssd_data->dnssd_name);
    error = avahi_entry_group_add_service_subtype(
        g_options.dnssd_data->ipp_ref,
        (g_options.interface ? (int)if_nametoindex(g_options.interface)
                             : AVAHI_IF_UNSPEC),
        AVAHI_PROTO_UNSPEC, 0, g_options.dnssd_data->dnssd_name, "_http._tcp", NULL,
        "_printer._sub._http._tcp");
    if (error)
      ERR("Error registering subtype for web interface of %s "
          "(_printer._sub._http._tcp): %d",
          g_options.dnssd_data->dnssd_name, error);
    else
      NOTE(
          "Registered subtype for web interface of %s "
          "(_printer._sub._http._tcp).",
          g_options.dnssd_data->dnssd_name);
  }

  avahi_entry_group_commit(g_options.dnssd_data->ipp_ref);
  avahi_string_list_free(ipp_txt);

  scanner = (ippScanner*) calloc(1, sizeof(ippScanner));
  if (is_scanner_present(scanner, g_options.real_port) == 0 || scanner == NULL)
     goto noscanner;
  /*
   * Create the TXT record for scanner ...
   */
  uscan_txt = NULL;
  if (scanner->representation)
     uscan_txt = avahi_string_list_add_printf(uscan_txt, "representation=%s", scanner->representation);
  else if (printer->representation)
     uscan_txt = avahi_string_list_add_printf(uscan_txt, "representation=%s", printer->representation);
  uscan_txt = avahi_string_list_add_printf(uscan_txt, "note=");
  if (scanner->uuid)
     uscan_txt = avahi_string_list_add_printf(uscan_txt, "UUID=%s", scanner->uuid);
  else if (printer->uuid)
     uscan_txt = avahi_string_list_add_printf(uscan_txt, "UUID=%s", printer->uuid);
  if (scanner->adminurl)
     uscan_txt = avahi_string_list_add_printf(uscan_txt, "adminurl=%s", scanner->adminurl);
  else if (printer->adminurl)
     uscan_txt = avahi_string_list_add_printf(uscan_txt, "adminurl=%s", printer->adminurl);
  else
     uscan_txt = avahi_string_list_add_printf(uscan_txt, "adminurl=%s", temp);
  uscan_txt = avahi_string_list_add_printf(uscan_txt, "duplex=%s", scanner->duplex);
  uscan_txt = avahi_string_list_add_printf(uscan_txt, "cs=%s", scanner->cs);
  uscan_txt = avahi_string_list_add_printf(uscan_txt, "pdl=%s", scanner->pdl);
  uscan_txt = avahi_string_list_add_printf(uscan_txt, "ty=%s", scanner->ty);
  uscan_txt = avahi_string_list_add_printf(uscan_txt, "rs=eSCL");
  uscan_txt = avahi_string_list_add_printf(uscan_txt, "vers=%s", scanner->vers);
  uscan_txt = avahi_string_list_add_printf(uscan_txt, "txtvers=1");


 /*
  * Register _uscan._tcp (LPD) with port 0 to reserve the service name...
  */

  NOTE("Registering scanner %s on interface %s for DNS-SD broadcasting ...",
       scanner->ty, g_options.interface);

  if (g_options.dnssd_data->uscan_ref == NULL)
    g_options.dnssd_data->uscan_ref =
      avahi_entry_group_new(g_options.dnssd_data->DNSSDClient,
			    dnssd_callback_uscan, NULL);

  if (g_options.dnssd_data->uscan_ref == NULL) {
    ERR("Could not establish Avahi entry group");
    avahi_string_list_free(uscan_txt);
    scanner = free_scanner(scanner);
    goto noscanner;
  }

  error =
    avahi_entry_group_add_service_strlst(g_options.dnssd_data->uscan_ref,
					 (g_options.interface ?
					  (int)if_nametoindex(g_options.interface) :
					  AVAHI_IF_UNSPEC),
					 AVAHI_PROTO_UNSPEC, 0,
					 g_options.dnssd_data->dnssd_name,
					 "_uscan._tcp", NULL, NULL,
					 g_options.real_port, uscan_txt);
  if (error) {
    ERR("Error registering %s as Unix scanner (_uscan._tcp): %d", scanner->ty,
	error);
    scanner = free_scanner(scanner);
    goto noscanner;
  }else
    NOTE("Registered %s as Unix scanner (_uscan._tcp).", scanner->ty);

 /*
  * Commit it scanner ...
  */

  avahi_entry_group_commit(g_options.dnssd_data->uscan_ref);

  avahi_string_list_free(uscan_txt);
  scanner = free_scanner(scanner);
noscanner:
  printer = free_printer(printer);
  return 0;
}

int dnssd_register(AvahiClient *c)
{
  //AvahiStringList *ipp_txt;             // DNS-SD IPP TXT record
  char            temp[256];            /* Subtype service string */
/*
  char            dnssd_name[1024];
  char            *dev_id = NULL;
  const char      *make;                // I - Manufacturer
  const char      *model;               // I - Model name
  const char      *serial = NULL;
  char            *ptr;
  int             error;
*/
  pthread_t       thread_escl;

 /*
  * Parse the device ID for MFG, MDL, and CMD
  */

  /*
  dev_id = strdup(g_options.device_id);
  NOTE("%s", "=======================================");
  NOTE("%s", dev_id);
  NOTE("%s", "=======================================");
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
  ptr = strchr(make, ';');
  if (ptr) *ptr = '\0';
  ptr = strchr(model, ';');
  if (ptr) *ptr = '\0';
  if (serial) {
    ptr = strchr(serial, ';');
    if (ptr) *ptr = '\0';
  }
  */
 /*
  * Additional printer properties
  */

  // snprintf(temp, sizeof(temp), "http://localhost:%d/", g_options.real_port);
  /*if (serial)
    snprintf(dnssd_name, sizeof(dnssd_name), "%s [%s]", model, serial);
  else
    snprintf(dnssd_name, sizeof(dnssd_name), "%s", model);
  g_options.dnssd_data->dnssd_name = strdup(dnssd_name);
  */
 /*
  * Create the TXT record for printer ...
  */

//  UUID=cfe92100-67c4-11d4-a45f-f8d0273ebac3" "note=" "adminurl=http://EPSON3EBAC3.local.:80/PRESENTATION/BONJOUR"
/*
  ipp_txt = NULL;
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "rp=ipp/print");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "priority=60");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "txtvers=1");
  ipp_txt = avahi_string_list_add_printf(ipp_txt, "qtotal=1");
  // free(dev_id);
*/
 //
  // Register _printer._tcp (LPD) with port 0 to reserve the service name...
  ///
/*
  NOTE("Registering printer %s on interface %s for DNS-SD broadcasting ...",
       dnssd_name, g_options.interface);
  if (c)
     g_options.dnssd_data->DNSSDClient = c;
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

 //
  // Then register the _ipp._tcp (IPP)...
  //

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
        "_print._sub._ipp._tcp");
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

 //
  // Finally _http.tcp (HTTP) for the web interface...
  //

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
*/
 /*
  * Commit it printer ...
  */

  // avahi_entry_group_commit(g_options.dnssd_data->ipp_ref);


  pthread_create (&thread_escl, NULL, dnssd_escl_register, c);

  return 0;
}

void dnssd_unregister()
{
  if (g_options.dnssd_data->ipp_ref) {
    avahi_entry_group_free(g_options.dnssd_data->ipp_ref);
    g_options.dnssd_data->ipp_ref = NULL;
  }
  if (g_options.dnssd_data->uscan_ref) {
    avahi_entry_group_free(g_options.dnssd_data->uscan_ref);
    g_options.dnssd_data->uscan_ref = NULL;
  }
}
