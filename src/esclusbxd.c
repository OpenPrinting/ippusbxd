#ifndef _DEFAULT_SOURCE
# define _DEFAULT_SOURCE
#endif

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/thread-watch.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <unistd.h>
#include "capabilities.h"
#include "logging.h"

static int terminate = 0;
static int real_port = 0;
static AvahiClient       *DNSSDClient;
// static AvahiThreadedPoll *DNSSDMaster;
static AvahiSimplePoll *DNSSDMaster;
static AvahiEntryGroup   *uscan_ref;

static int dnssd_escl_init();
static void dnssd_escl_shutdown();
static void dnssd_escl_unregister();

/*
 * 'dnssd_callback()' - Handle DNS-SD registration events generic.
 */

static void
dnssd_callback(AvahiEntryGroup      *g,         /* I - Service */
               AvahiEntryGroupState state)      /* I - Registration state */
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
    terminate = 1;
    break;
  case AVAHI_ENTRY_GROUP_UNCOMMITED:
  case AVAHI_ENTRY_GROUP_REGISTERING:
  default:
    break;
  }
}

/*
 * 'dnssd_callback()' - Handle DNS-SD registration events uscan.
 */

static void
dnssd_callback_uscan(AvahiEntryGroup      *g,   /* I - Service */
               AvahiEntryGroupState state,      /* I - Registration state */
               void                 *context)   /* I - Printer */
{
  (void)context;

  if (g == NULL || (uscan_ref != NULL && uscan_ref != g))
    return;
  dnssd_callback(g, state);
}

void* dnssd_register_escl(AvahiClient *c)
{
  AvahiStringList *uscan_txt;             // DNS-SD USCAN TXT record
  char            temp[256];            // Subtype service string
  int             error;
  ippScanner      *scanner = NULL;

  if (DNSSDClient) return NULL;
  snprintf(temp, sizeof(temp), "http://localhost:%d/", real_port);
  NOTE("%s", temp);
  scanner = (ippScanner*) calloc(1, sizeof(ippScanner));
  if (is_scanner_present(scanner, temp) == 0 || scanner == NULL)
     goto noscanner;

  /*
   * Create the TXT record for scanner ...
   */

  uscan_txt = NULL;
  uscan_txt = avahi_string_list_add_printf(uscan_txt, "representation=%s", scanner->representation);
  uscan_txt = avahi_string_list_add_printf(uscan_txt, "note=");
  uscan_txt = avahi_string_list_add_printf(uscan_txt, "UUID=%s", scanner->uuid);
  uscan_txt = avahi_string_list_add_printf(uscan_txt, "adminurl=%s", scanner->adminurl);
  uscan_txt = avahi_string_list_add_printf(uscan_txt, "dupplex=%s", scanner->duplex);
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
       scanner->ty, AVAHI_IF_UNSPEC);

  if (uscan_ref == NULL)
    uscan_ref =
      avahi_entry_group_new(c,
                            dnssd_callback_uscan, NULL);

  if (uscan_ref == NULL) {
    ERR("Could not establish Avahi entry group");
    avahi_string_list_free(uscan_txt);
    scanner = free_scanner(scanner);
    goto noscanner;
  }

  error =
    avahi_entry_group_add_service_strlst(uscan_ref,
                                         AVAHI_IF_UNSPEC,
                                         AVAHI_PROTO_UNSPEC, 0,
                                         scanner->ty,
                                         "_uscan._tcp", NULL, NULL,
                                         real_port, uscan_txt);
  if (error) {
    ERR("Error registering %s as Unix scanner (_uscan._tcp): %d", scanner->ty,
        error);
    avahi_string_list_free(uscan_txt);
    goto noscanner;
  }
  else
    NOTE("Registered %s as Unix scanner (_uscan._tcp).", scanner->ty);

 /*
  * Commit it scanner ...
  */

  avahi_entry_group_commit(uscan_ref);

  avahi_string_list_free(uscan_txt);
  scanner = free_scanner(scanner);
  return NULL;
noscanner:
  dnssd_escl_shutdown();
  return NULL;
}

static void dnssd_escl_unregister()
{
  if (uscan_ref) {
    avahi_entry_group_free(uscan_ref);
    uscan_ref = NULL;
  }
}

/*
 * 'dnssd_client_cb()' - Client callback for Avahi.
 *
 * Called whenever the client or server state changes...
 */

static void
dnssd_client_cb(AvahiClient      *c,            /* I - Client */
                AvahiClientState state,         /* I - Current state */
                void             *userdata)     /* I - User data (unused) */
{
  (void)userdata;
  int error;                    /* Error code, if any */

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
    dnssd_register_escl(c);
    break;

  case AVAHI_CLIENT_S_REGISTERING:
  case AVAHI_CLIENT_S_COLLISION:
    NOTE("Dropping printer registration because of possible host name change.");
    if (uscan_ref)
      avahi_entry_group_reset(uscan_ref);
    break;

  case AVAHI_CLIENT_FAILURE:
    if (avahi_client_errno(c) == AVAHI_ERR_DISCONNECTED) {
      NOTE("Avahi server disappeared, unregistering printer");
      dnssd_escl_unregister();
      /* Renewing client */
      if (DNSSDClient)
        avahi_client_free(DNSSDClient);
      if ((DNSSDClient =
           // avahi_client_new(avahi_threaded_poll_get
           avahi_client_new(avahi_simple_poll_get
                            (DNSSDMaster),
                            AVAHI_CLIENT_NO_FAIL,
                            dnssd_client_cb, NULL, &error)) == NULL) {
        ERR("Error: Unable to initialize DNS-SD client.");
        terminate = 1;
      }
    } else {
      ERR("Avahi server connection failure: %s",
          avahi_strerror(avahi_client_errno(c)));
      terminate = 1;
    }
    break;

  }
}

static void dnssd_escl_shutdown()
{
  if (DNSSDMaster) {
    // avahi_threaded_poll_stop(DNSSDMaster);
    avahi_simple_poll_quit(DNSSDMaster);
    dnssd_escl_unregister();
  }

  if (DNSSDClient) {
    avahi_client_free(DNSSDClient);
    DNSSDClient = NULL;
  }

  if (DNSSDMaster) {
    //avahi_threaded_poll_free(DNSSDMaster);
    avahi_simple_poll_free(DNSSDMaster);
    
    DNSSDMaster = NULL;
  }

  NOTE("DNS-SD shut down.");
}

static int dnssd_escl_init()
{
  int error; /* Error code, if any */

  DNSSDMaster = NULL;
  DNSSDClient = NULL;
  uscan_ref = NULL;
  
  if ((DNSSDMaster = avahi_simple_poll_new()) == NULL) {
  // if ((DNSSDMaster = avahi_threaded_poll_new()) == NULL) {
    ERR("Error: Unable to initialize DNS-SD.");
    goto fail;
  }

  if ((DNSSDClient = avahi_client_new(
           avahi_simple_poll_get(DNSSDMaster),
           // avahi_threaded_poll_get(DNSSDMaster),
           AVAHI_CLIENT_NO_FAIL, dnssd_client_cb, NULL, &error)) == NULL) {
    ERR("Error: Unable to initialize DNS-SD client.");
    goto fail;
  }

  // avahi_threaded_poll_start(DNSSDMaster);
  avahi_simple_poll_loop(DNSSDMaster);
  NOTE("DNS-SD initialized.");

  return 0;

 fail:
  dnssd_escl_shutdown();

  return -1;
}

int main(int argc, char **argv) {

    if (argc != 2) return -1;
    sleep(4);
    real_port = atoi(argv[1]);
    curl_global_init(CURL_GLOBAL_ALL);
    dnssd_escl_init();

    // Run the main loop
    avahi_simple_poll_loop(DNSSDMaster);
//    while(1) sleep(1);
    curl_global_cleanup();
    return 0;
}

