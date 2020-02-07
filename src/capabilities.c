#ifndef _DEFAULT_SOURCE
# define _DEFAULT_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <wchar.h>
#include <libxml/tree.h>
#include <libxml/parser.h>
#include <curl/curl.h>
#include "capabilities.h"
#include "logging.h"

struct cap
{
    char *memory;
    size_t size;
};

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

/**
 * \fn static size_t memory_callback_c(void *contents, size_t size, size_t nmemb, void *userp)
 * \brief Callback function that stocks in memory the content of the scanner capabilities.
 *
 * \return realsize (size of the content needed -> the scanner capabilities)
 */
static size_t
memory_callback_c(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct cap *mem = (struct cap *)userp;

    char *str = realloc(mem->memory, mem->size + realsize + 1);
    if (str == NULL) {
        fprintf(stderr, "not enough memory (realloc returned NULL)\n");
        return (0);
    }
    mem->memory = str;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size = mem->size + realsize;
    mem->memory[mem->size] = 0;
    return (realsize);
}
 
int
is_scanner_present(ippScanner *scanner, const char *name) {
    xmlDocPtr doc;
    xmlNodePtr racine;
    CURL *curl_handle = NULL;
    struct cap *var = NULL;
    char tmp[1024] = { 0 };
    int pass = 0;
    NOTE("is_scanner_present");
    if (!scanner || name[0] == 0) return 0;
    NOTE("go is_scanner_present");
    const char *scanner_capabilities = "eSCL/ScannerCapabilities";

    var = (struct cap *)calloc(1, sizeof(struct cap));
    NOTE("1 - is_scanner_present");
    if (var == NULL)
      return 0;
    NOTE("2 - is_scanner_present");
    var->memory = malloc(1);
    var->size = 0;
    curl_handle = curl_easy_init();
    strcpy(tmp, name);
    strcat(tmp, scanner_capabilities);
    NOTE("3 - is_scanner_present %s", tmp);
    curl_easy_setopt(curl_handle, CURLOPT_URL, tmp);
    if (strncmp(name, "https", 5) == 0) {
        NOTE("4 - is_scanner_present SSL");
        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    NOTE("5 - is_scanner_present");
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, memory_callback_c);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)var);
new_escl:
    sleep(3);
    if (curl_easy_perform(curl_handle) != CURLE_OK) {
        pass++;
        if (pass < 5)
          goto new_escl;
        NOTE("6 - is_scanner_present URL ERROR");
        fprintf(stderr, "Error : Curl\n");
        return 0;
    }
    // Ouverture du fichier XML
    doc = xmlReadMemory(var->memory, var->size, "ScannerCapabilities.xml", NULL, 0); //xmlParseFile("ScannerCapabilities.xml");
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
    //     NOTE("Document XML OK\n");

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
