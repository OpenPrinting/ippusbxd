#ifndef _DEFAULT_SOURCE
# define _DEFAULT_SOURCE
#endif

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <libxml/tree.h>
#include <libxml/parser.h>
#include <cups/cups.h>
#include "capabilities.h"
#include "logging.h"
#include "options.h"

struct cap
{
    char *memory;
    size_t size;
};

#define SIZE_DATA 32784

typedef void (*fct_parcours_t)(xmlNodePtr, ippScanner *ippscanner);

void parcours_prefixe(xmlNodePtr noeud, fct_parcours_t f, ippScanner *ippscanner);
void afficher_noeud(xmlNodePtr noeud, ippScanner *ippscanner);

void parcours_prefixe(xmlNodePtr noeud, fct_parcours_t f, ippScanner *ippscanner) {
    xmlNodePtr n;
    
    for (n = noeud; n != NULL; n = n->next) {
        f(n, ippscanner);
        if ((n->type == XML_ELEMENT_NODE) && (n->children != NULL)) 
            parcours_prefixe(n->children, f, ippscanner);
    }
}

int is_array(const char *name, ippScanner *ippscanner) {
    if (!strcmp(name, "Platen") || !strcmp(name, "Adf")) {
      char is[256] = { 0 };
      if (!strcmp(name, "Platen"))
        snprintf(is, sizeof(is), "platen");
      else if (!strcmp(name, "Adf"))
        snprintf(is, sizeof(is), "adf");
      if (is[0]) {
         if (ippscanner->is) {
            if (!strstr(ippscanner->is, is)) {
              int len = (strlen(ippscanner->is) + strlen(is) + 2);
              ippscanner->is = (char *) realloc(ippscanner->is, len);
              strcat(ippscanner->is, ",");
              strcat(ippscanner->is, is);
            }
         }
         else
            ippscanner->is = strdup(is);
      }
      return 1;
    }
    if (!strcmp(name, "AdfDuplexInputCaps")) {
      ippscanner->duplex = strdup("T");
      return 1;
    }
    if (!strcmp(name, "ScannerCapabilities") ||
        !strcmp(name, "SharpenSupport") ||
        !strcmp(name, "SupportedIntents") ||
        !strcmp(name, "CcdChannels") ||
        !strcmp(name, "ColorSpaces") ||
        !strcmp(name, "ColorModes") ||
        !strcmp(name, "DiscreteResolutions") ||
        !strcmp(name, "SupportedResolutions") ||
        !strcmp(name, "DocumentFormats") ||
        !strcmp(name, "ContentTypes") ||
        !strcmp(name, "DiscreteResolution") ||
        !strcmp(name, "CompressionFactorSupport") ||
        !strcmp(name, "SupportedMediaTypes") ||
        !strcmp(name, "SettingProfiles") ||
        !strcmp(name, "SettingProfile") ||
        !strcmp(name, "PlatenInputCaps"))
           return 1;
        return 0;
 }
    
void set_value_escl_scanner(const char *noeud, const char *contenu, ippScanner *ippscanner)
{
    if (!strcmp(noeud, "Version")) {
      ippscanner->vers = strdup(contenu);
    } else if (!strcmp(noeud, "MakeAndModel")) {
      ippscanner->ty = strdup(contenu);
    } else if (!strcmp(noeud, "UUID")) {
      ippscanner->uuid = strdup(contenu);
    } else if (!strcmp(noeud, "AdminURI")) {
      ippscanner->adminurl = strdup(contenu);
    } else if (!strcmp(noeud, "IconURI")) {
      ippscanner->representation = strdup(contenu);
    } else if (!strcmp(noeud, "DocumentFormat")){
      if (ippscanner->pdl) {
         if (!strstr(ippscanner->pdl, contenu)) {
            int len = (strlen(ippscanner->pdl) + strlen(contenu) + 2);
            ippscanner->pdl = (char *) realloc(ippscanner->pdl, len);
            strcat(ippscanner->pdl, ",");
            strcat(ippscanner->pdl, contenu);
         }
      }
      else {
         ippscanner->pdl = strdup(contenu);
      }
    } else if (!strcmp(noeud, "ColorMode")){
      char modecolor[256] = { 0 };
      if (!strcmp(contenu, "Grayscale8"))
        snprintf(modecolor, sizeof(modecolor), "grayscale");
      else if (!strcmp(contenu, "RGB24"))
        snprintf(modecolor, sizeof(modecolor), "color");
      else if (!strcmp(contenu, "BlackAndWhite1"))
        snprintf(modecolor, sizeof(modecolor), "binary");
      if (modecolor[0]) {
         if (ippscanner->cs) {
            if (!strstr(ippscanner->cs, modecolor)) {
              int len = (strlen(ippscanner->cs) + strlen(modecolor) + 2);
              ippscanner->cs = (char *) realloc(ippscanner->cs, len);
              strcat(ippscanner->cs, ",");
              strcat(ippscanner->cs, modecolor);
            }
         }
         else {
            ippscanner->cs = strdup(modecolor);
         }
      }
    }
}

void afficher_noeud(xmlNodePtr noeud, ippScanner *ippscanner) {
    if (is_array((const char*)noeud->name, ippscanner)) return;
    if (noeud->type == XML_ELEMENT_NODE) {
        xmlChar *chemin = xmlGetNodePath(noeud);
        if (noeud->children != NULL && noeud->children->type == XML_TEXT_NODE) {
            xmlChar *contenu = xmlNodeGetContent(noeud);
	    if (noeud->name != NULL)
              set_value_escl_scanner((const char*)noeud->name , (const char*)contenu, ippscanner);
            xmlFree(contenu);
        }
        xmlFree(chemin);
    }
}

static int
value_papersize_less(int p1w, int p1h, int p2w, int p2h) {
	return ((p1w < p2w && p1h <= p2h) || (p1h < p2h && p1w <= p2w));
}

// Classify paper size according to Apple Bonjour rules
// Returns:
//     ">isoC-A2" for paper larger that C or A2
//     "isoC-A2" for C or A2 paper
//     "tabloid-A3" for Tabloid or A3 paper
//     "legal-A4" for Legal or A4 paper
//     "<legal-A4" for paper smaller that Legal or A4
static char *get_format(int x_dim_max, int y_dim_max)
{
        if (x_dim_max == 0 || y_dim_max == 0) {
                return NULL;
        }

        // Now classify by printer size
        //                  US name      US inches   US mm           ISO mm
        //   "legal-A4"     A, Legal     8.5 x 14    215.9 x 355.6   A4: 210 x 297
        //   "tabloid-A3"   B, Tabloid   11 x 17     279.4 x 431.8   A3: 297 x 420
        //   "isoC-A2"      C            17 × 22     431.8 × 558,8   A2: 420 x 594
        //
        // Please note, Apple in the "Bonjour Printing Specification"
        // incorrectly states paper sizes as 9x14, 13x19 and 18x24 inches

        int legal_x   = 21590,
            legal_y   = 35560,
            a4_x      = 21000,
            a4_y      = 29700,
            tabloid_x = 27940,
            tabloid_y = 43180,
            a3_x      = 29700,
            a3_y      = 42000,
            isoC_x    = 43180,
            isoC_y    = 55880,
            a2_x      = 42000,
            a2_y      = 59400;

	if (value_papersize_less(isoC_x, x_dim_max, isoC_y, y_dim_max) ||
	    value_papersize_less(a2_x, x_dim_max, a2_y, y_dim_max))
		return strdup(">isoC-A2");
	else if (!value_papersize_less(x_dim_max, isoC_x, y_dim_max, isoC_y) ||
	         !value_papersize_less(x_dim_max, a2_x, y_dim_max, a2_y))
		return strdup("isoC-A2");
	else if (!value_papersize_less(x_dim_max, tabloid_x, y_dim_max, tabloid_y) ||
	         !value_papersize_less(x_dim_max, a3_x, y_dim_max, a3_y))
		return strdup("tabloid-A3");
	else if (!value_papersize_less(x_dim_max, legal_x, y_dim_max, legal_y) ||
	         !value_papersize_less(x_dim_max, a4_x, y_dim_max, a4_y))
		return strdup("legal-A4");
	else
		return strdup("<legal-A4");
}


char * get_format_paper(char *val)
{
   int x_dim_max = 0, x_dim = 0;
   int y_dim_max = 0, y_dim = 0;
   int a = 0;
   if (!val) return NULL;
   
   while ((val = strchr(val, '{'))) {
	       val++;
	       char test1[255] = { 0 };
	       char test2[255] = { 0 };
		   
	       char *tmp = strchr(val, '=');
	       if (!tmp) continue;
	       a = strlen(val) - strlen(tmp);
	       val+=(a + 1);
	       
	       tmp = strchr(val, ' ');
	       if (!tmp) continue;
	       a = strlen(val) - strlen(tmp);
	       strncpy(test2, val, a);
	       if (strchr(test2, '-') == NULL)
			   x_dim = atoi(test2);
	       val+=(a + 1);
		   
	       memset(test1, 0, 255);
	       memset(test2, 0, 255);
	       if (!tmp) continue;
	       tmp = strchr(val, '=');
	       a = strlen(val) - strlen(tmp);
	       val+=(a + 1);
	       
	       if (!tmp) continue;
	       tmp = strchr(val, '}');
	       if (!tmp) continue;
	       a = strlen(val) - strlen(tmp);
	       strncpy(test2, val, a);
	       if (strchr(test2, '-') == NULL)
			   y_dim = atoi(test2);
			   
		   if (x_dim_max < x_dim)
		      x_dim_max = x_dim;
		   if (y_dim_max < y_dim)
		      y_dim_max = y_dim;
		   val+=(a + 1);
   }
   return get_format(x_dim_max, y_dim_max);
}

static char *
_search_tag(char *buffer, const char *tag)
{
    char *result = NULL;
    char *tmp = strstr(buffer, tag);
    if (tmp) {
       char *c = strchr(tmp, ';');
       *c = 0;
       result = strdup(tmp + 4);
       *c = ';';
    }
    return result;
}

int
ipp_request(ippPrinter *printer, int port)
{
  http_t	*http = NULL; 
  ipp_t *request, *response = NULL;
  ipp_attribute_t *attr;
  char uri[1024];
  char buffer[1024];

  /* Try to connect to IPP server */
  if ((http = httpConnect2("127.0.0.1", port, NULL, AF_UNSPEC,
			   HTTP_ENCRYPTION_IF_REQUESTED, 1, 30000, NULL)) == NULL) {
    printf("Unable to connect to 127.0.0.1 on port %d.\n", port);
    return 1;
  }

  snprintf(uri, sizeof(uri), "http://127.0.0.1:%d/ipp/print", port);

  /* Fire a Get-Printer-Attributes request */
  request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
  ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri",
	             NULL, uri);
  response = cupsDoRequest(http, request, "/ipp/print");

  /* Print the attributes received from the IPP printer */
  attr = ippFirstAttribute(response);
  while (attr) {
    /* Convert IPP attribute's value to string */
    ippAttributeString(attr, buffer, sizeof(buffer));
    char *attr_name = (char*)ippGetName(attr);
    if (!attr_name) continue;
    if (!strcasecmp(attr_name, "printer-icons"))
       printer->representation = strdup(buffer);
    else if (!strcasecmp(attr_name, "printer-device-id")) {
//     usb_MDL:          MDL, extracted from "printer-device-id"
//     usb_MFG:          MFG, extracted from "printer-device-id"
//     usb_CMD:          CMD, extracted from "printer-device-id"
//     MFG:Brother;CMD:PJL,HBP,URF;MDL:DCP-L2530DW series;CLS:PRINTER;CID:Brother Laser Type1;URF:W8,CP1,IS4-1,MT1-3-4-5-8,OB10,PQ3-4-5,RS300-600-1200,V1.4,DM1;[en]
       printer->mfg = _search_tag(buffer, "MFG:");
       printer->mdl = _search_tag(buffer, "MDL:");
       printer->cmd = _search_tag(buffer, "CMD:");
    }
    else if(!strcasecmp(attr_name, "printer-uuid"))
       printer->uuid = strdup(buffer + 9);
    else if(!strcasecmp(attr_name, "printer-more-info"))
       printer->adminurl = strdup(buffer);
    else if(!strcasecmp(attr_name, "mopria-certified"))
       printer->mopria_certified = strdup(buffer);
    else if(!strcasecmp(attr_name, "printer-kind"))
       printer->kind = strdup(buffer);
    else if(!strcasecmp(attr_name, "color-supported")) {
       if(!strcasecmp(buffer, "true"))
           printer->color = strdup("T");
       else
           printer->color = strdup("F");
    }
    else if(!strcasecmp(attr_name, "sides-supported")) {
       if(strcasestr(buffer, "two-"))
           printer->side = strdup("T");
       else if(strcasestr(buffer, "one-"))
           printer->side = strdup("F");
       else
           printer->side = strdup("U");
    }
    else if(!strcasecmp(attr_name, "printer-location"))
       printer->note = strdup(buffer);
    else if(!strcasecmp(attr_name, "printer-make-and-model"))
       printer->ty = strdup(buffer);
    else if(!strcasecmp(attr_name, "document-format-supported"))
       printer->pdl = strdup(buffer);
    else if(!strcasecmp(attr_name, "urf-supported"))
       printer->urf = strdup(buffer);
    else if(!strcasecmp(attr_name, "media-size-supported"))
       printer->papermax = get_format_paper(buffer);
    /* next attribute */
    attr = ippNextAttribute(response);
  }
  if (!strcasecmp(httpGetField(http, HTTP_FIELD_CONNECTION), "close"))
  {
      httpClearFields(http);
      if (httpReconnect2(http, 30000, NULL))
      {
          goto close_http;
      }
  }
  httpClearFields(http);
  httpSetField(http, HTTP_FIELD_AUTHORIZATION, httpGetAuthString(http));
  httpSetField(http, HTTP_FIELD_ACCEPT_LANGUAGE, "en");
  if (!httpHead(http, "/ipp/faxout"))
  {
    printer->fax = strdup("T");
  }
close_http:
  httpClose(http);
  return 0;
}

ippPrinter *
free_printer(ippPrinter *printer)
{
   if (!printer) return NULL;
   free(printer->representation);
   free(printer->uuid);
   free(printer->adminurl);
   free(printer->mopria_certified);
   free(printer->kind);
   free(printer->papermax);
   free(printer->urf);
   free(printer->color);
   free(printer->note);
   free(printer->pdl);
   free(printer->ty);
   free(printer->side);
   free(printer->fax);
   free(printer);
   return NULL;
}

static char *
http_request(const char *hostname, const char *resource, int port, int *size_data)
{
  http_t	*http = NULL;		/* HTTP connection */
  http_status_t	status = HTTP_STATUS_OK;			/* Status of GET command */
  char		buffer[SIZE_DATA] = { 0 };		/* Input buffer */
  long		bytes;			/* Number of bytes read */
  off_t		total;		        /* Total bytes */
  const char	*encoding;		/* Negotiated Content-Encoding */
  char *memory = (char*)calloc(1, sizeof (char));
  char *tmp = NULL;

  http = httpConnect2(hostname, port, NULL, AF_UNSPEC, HTTP_ENCRYPTION_NEVER, 1, 30000, NULL);
  if (http == NULL)
  {
    perror(hostname);
    return NULL;
  }
  NOTE("Checking file \"%s\"...\n", resource);

  do
  {
    if (!strcasecmp(httpGetField(http, HTTP_FIELD_CONNECTION), "close"))
    {
      	httpClearFields(http);
      	if (httpReconnect2(http, 30000, NULL))
      	{
      		  status = HTTP_STATUS_ERROR;
      		  break;
      	}
    }

    httpClearFields(http);
    httpSetField(http, HTTP_FIELD_AUTHORIZATION, httpGetAuthString(http));
    httpSetField(http, HTTP_FIELD_ACCEPT_LANGUAGE, "en");

    if (httpGet(http, resource))
    {
      if (httpReconnect2(http, 30000, NULL))
      {
        status = HTTP_STATUS_ERROR;
        break;
      }
      else
      {
        status = HTTP_STATUS_UNAUTHORIZED;
        continue;
      }
    }

    while ((status = httpUpdate(http)) == HTTP_STATUS_CONTINUE);

    if (status == HTTP_STATUS_UNAUTHORIZED)
    {
     /*
      * Flush any error message...
      */

      httpFlush(http);

     /*
      * See if we can do authentication...
      */

      if (cupsDoAuthentication(http, "HEAD", resource))
      {
        status = HTTP_STATUS_CUPS_AUTHORIZATION_CANCELED;
        break;
      }

      if (httpReconnect2(http, 30000, NULL))
      {
        status = HTTP_STATUS_ERROR;
        break;
      }

      continue;
    }
  }
  while (status == HTTP_STATUS_UNAUTHORIZED ||
         status == HTTP_STATUS_UPGRADE_REQUIRED);

  if (status == HTTP_STATUS_OK)
    NOTE("HEAD OK:");
  else
    NOTE("HEAD failed with status %d...\n", status);

  encoding = httpGetContentEncoding(http);

  NOTE("Requesting file \"%s\" (Accept-Encoding: %s)...\n", resource,
         encoding ? encoding : "identity");


  do
  {
    if (!strcasecmp(httpGetField(http, HTTP_FIELD_CONNECTION), "close"))
    {
      httpClearFields(http);
      if (httpReconnect2(http, 30000, NULL))
      {
        status = HTTP_STATUS_ERROR;
        break;
      }
    }

    httpClearFields(http);
    httpSetField(http, HTTP_FIELD_AUTHORIZATION, httpGetAuthString(http));
    httpSetField(http, HTTP_FIELD_ACCEPT_LANGUAGE, "en");
    httpSetField(http, HTTP_FIELD_ACCEPT_ENCODING, encoding);

    if (httpGet(http, resource))
    {
      if (httpReconnect2(http, 30000, NULL))
      {
        status = HTTP_STATUS_ERROR;
        break;
      }
      else
      {
        status = HTTP_STATUS_UNAUTHORIZED;
        continue;
      }
    }

    while ((status = httpUpdate(http)) == HTTP_STATUS_CONTINUE);


    if (status == HTTP_STATUS_UNAUTHORIZED)
    {
     /*
      * Flush any error message...
      */

      httpFlush(http);

     /*
      * See if we can do authentication...
      */


      if (cupsDoAuthentication(http, "GET", resource))
      {
        status = HTTP_STATUS_CUPS_AUTHORIZATION_CANCELED;
        break;
      }

      if (httpReconnect2(http, 30000, NULL))
      {
        status = HTTP_STATUS_ERROR;
        break;
      }

      continue;
    }
  }
  while (status == HTTP_STATUS_UNAUTHORIZED || status == HTTP_STATUS_UPGRADE_REQUIRED);

  if (status != HTTP_STATUS_OK)
    {
      NOTE("GET failed with status %d...\n", status);
      return NULL;
    }

  NOTE("GET success before read escl capabilities.\n");
  total = 0;
  while ((bytes = httpRead2(http, buffer, (SIZE_DATA - 1))) > 0)
  {
    char *str = realloc(memory, total + bytes + 1);
    memory = str;
    memcpy(&(memory[total]), buffer, bytes);
    total += bytes;
    memory[total] = 0;
    memset(buffer, 0, SIZE_DATA);
  }
  tmp = (char *)strstr(memory, "<?xml version");
  if (tmp)
  {
     char *tmp2 = NULL;
     total = strlen(tmp);
     tmp2 = strrchr(tmp, '>');
     if (tmp2)
     {
       int len = total - strlen(tmp2);
       tmp[len + 1] = 0;
       tmp2 = strdup(tmp);
       free(memory);
       memory = tmp2;
       total = strlen(memory);
     }
  }
  *size_data = total;
  httpClose(http);
  return memory;
}

int
is_scanner_present(ippScanner *scanner, int port) {
    xmlDocPtr doc;
    xmlNodePtr racine;
    char *memory = NULL;
    int size = 0;
    NOTE("is_scanner_present");
    if (!scanner) return 0;
    NOTE("go is_scanner_present");
    memory = http_request("127.0.0.1", "/eSCL/ScannerCapabilities", port, &size);
    NOTE("Capabilites[\n%s\n]\n", memory);
    // Ouverture du fichier XML
    doc = xmlReadMemory(memory, size, "ScannerCapabilities.xml", NULL, 0); //xmlParseFile("ScannerCapabilities.xml");
    if (doc == NULL) {
        NOTE("Document XML invalide\n");
        return 0; 
    }
    // Récupération de la racine
    racine = xmlDocGetRootElement(doc);
    if (racine == NULL) {
        NOTE("Document XML vierge\n");
        xmlFreeDoc(doc);
        return 0;
    }
    // Parcours
    parcours_prefixe(racine, afficher_noeud, scanner);
    if (!scanner->duplex) scanner->duplex = strdup("F");
    NOTE("txt = [\n\"representation=%s\"\n\"note=\"\n\"UUID=%s\"\n\"adminurl=%s\"\n\"duplex=%s\"\n\"is=%s\"\n\"cs=%s\"\n\"pdl=%s\"\n\"ty=%s\"\n\"rs=eSCL\"\n\"vers=%s\"\n\"txtvers=1\"\n]",
         scanner->representation, scanner->uuid, scanner->adminurl, scanner->duplex, scanner->is, scanner->cs, scanner->pdl, scanner->ty, scanner->vers);
    xmlFreeDoc(doc);

    return 1;
}

ippScanner *
free_scanner (ippScanner *scanner) {
  if (!scanner) return NULL;
  
  free(scanner->representation);
  free(scanner->uuid);
  free(scanner->adminurl);
  free(scanner->duplex);
  free(scanner->is);
  free(scanner->cs);
  free(scanner->pdl);
  free(scanner->ty);
  free(scanner->vers);
  free(scanner);
  return NULL;
}
