/* ============================================================
 * Client OPC UA FX con mDNS Discovery via Mini-Server Integrato
 *
 * Opzione 4: invece di chiamare FindServersOnNetwork verso un
 * LDS esterno, viene avviato un UA_Server minimale con mDNS
 * abilitato. Questo server ascolta i pacchetti multicast mDNS
 * sulla rete e notifica tramite callback ogni server scoperto.
 * Il client OPC UA usa poi gli URL raccolti per connettersi
 * e fare il browse della struttura UAFX.
 *
 * Flusso:
 *   [UA_Server mDNS listener] --callback--> [lista URL]
 *   [UA_Client] --connessione TCP--> [server UAFX scoperti]
 * ============================================================ */

#include "open62541.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>



/* ============================================================
 * Struttura per raccogliere i server scoperti via mDNS
 * ============================================================ */

#define MAX_DISCOVERED_SERVERS 16
#define MAX_URL_LEN            256

typedef struct {
    char   urls[MAX_DISCOVERED_SERVERS][MAX_URL_LEN];
    char   names[MAX_DISCOVERED_SERVERS][MAX_URL_LEN];
    size_t count;
} DiscoveryList;

/* ============================================================
 * Callback mDNS - chiamata dal server ogni volta che rileva
 * un nuovo annuncio mDNS sulla rete
 * ============================================================ */

static void
onServerOnNetwork(const UA_ServerOnNetwork *son,
                  UA_Boolean isServerAnnounce,
                  UA_Boolean isTxtReceived,
                  void *data) {

    DiscoveryList *list = (DiscoveryList *)data;

    /* Gestiamo solo gli annunci (non le cancellazioni) */
    if(!isServerAnnounce)
        return;

    /* Aspettiamo che sia arrivato anche il record TXT
     * (contiene capabilities e serverName completo) */
    if(!isTxtReceived)
        return;

    if(list->count >= MAX_DISCOVERED_SERVERS)
        return;

    /* Costruiamo l'URL dal discoveryUrl della struttura */
    size_t urlLen = son->discoveryUrl.length;
    if(urlLen == 0 || urlLen >= MAX_URL_LEN)
        return;

    /* Evita duplicati */
    for(size_t i = 0; i < list->count; i++) {
        if(strncmp(list->urls[i],
                   (char*)son->discoveryUrl.data,
                   urlLen) == 0)
            return;
    }

    /* Salva URL */
    memcpy(list->urls[list->count], son->discoveryUrl.data, urlLen);
    list->urls[list->count][urlLen] = '\0';

    /* Salva nome */
    size_t nameLen = son->serverName.length < MAX_URL_LEN - 1
                     ? son->serverName.length
                     : MAX_URL_LEN - 1;
    memcpy(list->names[list->count], son->serverName.data, nameLen);
    list->names[list->count][nameLen] = '\0';

    printf("  [+] Scoperto: %s  -->  %s\n",
           list->names[list->count],
           list->urls[list->count]);

    /* Stampa capabilities se presenti */
    if(son->serverCapabilitiesSize > 0) {
        printf("      Capabilities: ");
        for(size_t i = 0; i < son->serverCapabilitiesSize; i++) {
            printf("%.*s ",
                   (int)son->serverCapabilities[i].length,
                   son->serverCapabilities[i].data);
        }
        printf("\n");
    }

    list->count++;
}

/* ============================================================
 * Funzione di discovery mDNS tramite mini-server
 *
 * Avvia un UA_Server minimale sulla porta indicata con mDNS
 * abilitato, lo lascia girare per `waitSeconds` secondi
 * raccogliendo gli annunci, poi lo spegne.
 * ============================================================ */

static UA_StatusCode
runMdnsDiscovery(DiscoveryList *list, unsigned int waitSeconds,
                 UA_UInt16 listenerPort) {

#ifndef UA_ENABLE_DISCOVERY_MULTICAST
    printf("  ATTENZIONE: open62541 compilato senza UA_ENABLE_DISCOVERY_MULTICAST\n");
    printf("  Ricompilare con -DUA_ENABLE_DISCOVERY_MULTICAST=ON\n");
    return UA_STATUSCODE_BADNOTIMPLEMENTED;
#else

    UA_Server *server = UA_Server_new();
    if(!server)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    UA_ServerConfig *config = UA_Server_getConfig(server);

    /* Configurazione minima - porta diversa dal server UAFX
     * per non creare conflitti se gira sulla stessa macchina */
    UA_ServerConfig_setMinimal(config, listenerPort, NULL);

    /* Abilita mDNS */
    config->mdnsEnabled = UA_TRUE;

    /* Nome del listener mDNS (appare nella rete come servizio) */
    UA_String_clear(&config->mdnsConfig.mdnsServerName);
    config->mdnsConfig.mdnsServerName =
        UA_String_fromChars("UAFX-DiscoveryListener");

    /* Disabilita il logging verboso del server per non
     * sporcare l'output del discovery */

    /* Registra la callback: verrà chiamata ad ogni annuncio mDNS */
    UA_Server_setServerOnNetworkCallback(server, onServerOnNetwork, list);

    /* Avvio del server */
    UA_StatusCode retval = UA_Server_run_startup(server);
    if(retval != UA_STATUSCODE_GOOD) {
        printf("  Errore avvio mDNS listener: %s\n",
               UA_StatusCode_name(retval));
        UA_Server_delete(server);
        return retval;
    }

    /* Loop per `waitSeconds` secondi raccogliendo annunci mDNS.
     * UA_Server_run_iterate(server, false) = non-blocking:
     * processa gli eventi pendenti e ritorna subito. */
    printf("  Ascolto mDNS per %u secondi...\n\n", waitSeconds);

    UA_DateTime stopTime = UA_DateTime_nowMonotonic()
                           + (UA_DateTime)(waitSeconds) * UA_DATETIME_SEC;

    while(UA_DateTime_nowMonotonic() < stopTime) {
        UA_Server_run_iterate(server, false);
        UA_sleep_ms(50); /* polling ogni 50ms */
    }

    UA_Server_run_shutdown(server);
    UA_Server_delete(server);

    return UA_STATUSCODE_GOOD;
#endif
}

/* ============================================================
 * Helper Functions (invariate rispetto al client originale)
 * ============================================================ */

static void printSeparator(const char *title) {
    printf("\n");
    printf("════════════════════════════════════════════════════\n");
    if(title) {
        printf("  %s\n", title);
        printf("════════════════════════════════════════════════════\n");
    }
}

static char *readStringProperty(UA_Client *client, UA_NodeId parentNode,
                                const char *propertyName) {
    UA_BrowseRequest bReq;
    UA_BrowseRequest_init(&bReq);
    bReq.requestedMaxReferencesPerNode = 0;
    bReq.nodesToBrowse = UA_BrowseDescription_new();
    bReq.nodesToBrowseSize = 1;
    bReq.nodesToBrowse[0].nodeId = parentNode;
    bReq.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bReq.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse bResp = UA_Client_Service_browse(client, bReq);
    char *result = NULL;

    if(bResp.responseHeader.serviceResult == UA_STATUSCODE_GOOD) {
        for(size_t i = 0; i < bResp.resultsSize; i++) {
            for(size_t j = 0; j < bResp.results[i].referencesSize; j++) {
                UA_ReferenceDescription *ref = &bResp.results[i].references[j];
                UA_String propStr = UA_STRING((char*)propertyName);
                if(UA_String_equal(&ref->browseName.name, &propStr)) {
                    UA_Variant value;
                    UA_Variant_init(&value);
                    UA_StatusCode rc = UA_Client_readValueAttribute(
                        client, ref->nodeId.nodeId, &value);
                    if(rc == UA_STATUSCODE_GOOD &&
                       UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_STRING])) {
                        UA_String *str = (UA_String*)value.data;
                        result = (char*)malloc(str->length + 1);
                        memcpy(result, str->data, str->length);
                        result[str->length] = '\0';
                    }
                    UA_Variant_clear(&value);
                    break;
                }
            }
        }
    }

    UA_BrowseResponse_clear(&bResp);
    UA_BrowseRequest_clear(&bReq);
    return result;
}

/* ============================================================
 * Browse FunctionalEntity
 * ============================================================ */

static void browseFunctionalEntity(UA_Client *client, UA_NodeId feNodeId,
                                   const char *feName) {
    printf("\n      ┌─ FunctionalEntity: %s\n", feName);

    char *authorUri   = readStringProperty(client, feNodeId, "AuthorUri");
    char *identifier  = readStringProperty(client, feNodeId, "AuthorAssignedIdentifier");
    char *version     = readStringProperty(client, feNodeId, "AuthorAssignedVersion");

    if(authorUri)   { printf("      │  AuthorUri: %s\n",   authorUri);   free(authorUri); }
    if(identifier)  { printf("      │  Identifier: %s\n",  identifier);  free(identifier); }
    if(version)     { printf("      │  Version: %s\n",     version);     free(version); }

    UA_BrowseRequest bReq;
    UA_BrowseRequest_init(&bReq);
    bReq.requestedMaxReferencesPerNode = 0;
    bReq.nodesToBrowse = UA_BrowseDescription_new();
    bReq.nodesToBrowseSize = 1;
    bReq.nodesToBrowse[0].nodeId = feNodeId;
    bReq.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bReq.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse bResp = UA_Client_Service_browse(client, bReq);

    if(bResp.responseHeader.serviceResult == UA_STATUSCODE_GOOD) {
        for(size_t i = 0; i < bResp.resultsSize; i++) {
            for(size_t j = 0; j < bResp.results[i].referencesSize; j++) {
                UA_ReferenceDescription *ref = &bResp.results[i].references[j];
                UA_String outputDataStr = UA_STRING("OutputData");
                if(!UA_String_equal(&ref->browseName.name, &outputDataStr))
                    continue;

                printf("      │\n");
                printf("      └─ OutputData/\n");

                UA_BrowseRequest bReq2;
                UA_BrowseRequest_init(&bReq2);
                bReq2.requestedMaxReferencesPerNode = 0;
                bReq2.nodesToBrowse = UA_BrowseDescription_new();
                bReq2.nodesToBrowseSize = 1;
                bReq2.nodesToBrowse[0].nodeId = ref->nodeId.nodeId;
                bReq2.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
                bReq2.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

                UA_BrowseResponse bResp2 = UA_Client_Service_browse(client, bReq2);

                if(bResp2.responseHeader.serviceResult == UA_STATUSCODE_GOOD) {
                    for(size_t k = 0; k < bResp2.resultsSize; k++) {
                        for(size_t l = 0; l < bResp2.results[k].referencesSize; l++) {
                            UA_ReferenceDescription *dr = &bResp2.results[k].references[l];
                            UA_Variant value;
                            UA_Variant_init(&value);
                            UA_StatusCode rc = UA_Client_readValueAttribute(
                                client, dr->nodeId.nodeId, &value);
                            printf("         └─ %.*s",
                                   (int)dr->browseName.name.length,
                                   dr->browseName.name.data);
                            if(rc == UA_STATUSCODE_GOOD &&
                               UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_FLOAT])) {
                                UA_Float *fv = (UA_Float*)value.data;
                                printf(": %.2f", *fv);
                                char *units = readStringProperty(client, dr->nodeId.nodeId,
                                                                 "EngineeringUnits");
                                if(units) { printf(" %s", units); free(units); }
                                printf(" [Float]\n");
                            } else {
                                printf(" (read failed)\n");
                            }
                            UA_Variant_clear(&value);
                        }
                    }
                }
                UA_BrowseResponse_clear(&bResp2);
                UA_BrowseRequest_clear(&bReq2);
            }
        }
    }

    UA_BrowseResponse_clear(&bResp);
    UA_BrowseRequest_clear(&bReq);
}

/* ============================================================
 * Browse AutomationComponent
 * ============================================================ */

static void browseAutomationComponent(UA_Client *client, UA_NodeId acNodeId,
                                      const char *acName) {
    printf("\n    ┌─ AutomationComponent: %s\n", acName);

    char *conformanceName = readStringProperty(client, acNodeId, "ConformanceName");
    if(conformanceName) {
        printf("    │  ConformanceName: %s\n", conformanceName);
        free(conformanceName);
    }

    UA_BrowseRequest bReq;
    UA_BrowseRequest_init(&bReq);
    bReq.requestedMaxReferencesPerNode = 0;
    bReq.nodesToBrowse = UA_BrowseDescription_new();
    bReq.nodesToBrowseSize = 1;
    bReq.nodesToBrowse[0].nodeId = acNodeId;
    bReq.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bReq.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse bResp = UA_Client_Service_browse(client, bReq);

    if(bResp.responseHeader.serviceResult == UA_STATUSCODE_GOOD) {
        for(size_t i = 0; i < bResp.resultsSize; i++) {
            for(size_t j = 0; j < bResp.results[i].referencesSize; j++) {
                UA_ReferenceDescription *ref = &bResp.results[i].references[j];

                /* ── Assets ── */
                UA_String assetsStr = UA_STRING("Assets");
                if(UA_String_equal(&ref->browseName.name, &assetsStr)) {
                    printf("    │\n");
                    printf("    ├─ Assets/\n");

                    UA_BrowseRequest bReq2;
                    UA_BrowseRequest_init(&bReq2);
                    bReq2.requestedMaxReferencesPerNode = 0;
                    bReq2.nodesToBrowse = UA_BrowseDescription_new();
                    bReq2.nodesToBrowseSize = 1;
                    bReq2.nodesToBrowse[0].nodeId = ref->nodeId.nodeId;
                    bReq2.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
                    bReq2.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

                    UA_BrowseResponse bResp2 = UA_Client_Service_browse(client, bReq2);
                    if(bResp2.responseHeader.serviceResult == UA_STATUSCODE_GOOD) {
                        for(size_t k = 0; k < bResp2.resultsSize; k++) {
                            for(size_t l = 0; l < bResp2.results[k].referencesSize; l++) {
                                UA_ReferenceDescription *ar = &bResp2.results[k].references[l];
                                printf("    │  └─ %.*s\n",
                                       (int)ar->browseName.name.length,
                                       ar->browseName.name.data);
                                char *mfr    = readStringProperty(client, ar->nodeId.nodeId, "Manufacturer");
                                char *model  = readStringProperty(client, ar->nodeId.nodeId, "Model");
                                char *serial = readStringProperty(client, ar->nodeId.nodeId, "SerialNumber");
                                if(mfr)    { printf("    │     Manufacturer: %s\n", mfr);    free(mfr); }
                                if(model)  { printf("    │     Model: %s\n",        model);  free(model); }
                                if(serial) { printf("    │     SerialNumber: %s\n", serial); free(serial); }
                            }
                        }
                    }
                    UA_BrowseResponse_clear(&bResp2);
                    UA_BrowseRequest_clear(&bReq2);
                }

                /* ── FunctionalEntities ── */
                UA_String feStr = UA_STRING("FunctionalEntities");
                if(UA_String_equal(&ref->browseName.name, &feStr)) {
                    printf("    │\n");
                    printf("    └─ FunctionalEntities/\n");

                    UA_BrowseRequest bReq2;
                    UA_BrowseRequest_init(&bReq2);
                    bReq2.requestedMaxReferencesPerNode = 0;
                    bReq2.nodesToBrowse = UA_BrowseDescription_new();
                    bReq2.nodesToBrowseSize = 1;
                    bReq2.nodesToBrowse[0].nodeId = ref->nodeId.nodeId;
                    bReq2.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
                    bReq2.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

                    UA_BrowseResponse bResp2 = UA_Client_Service_browse(client, bReq2);
                    if(bResp2.responseHeader.serviceResult == UA_STATUSCODE_GOOD) {
                        for(size_t k = 0; k < bResp2.resultsSize; k++) {
                            for(size_t l = 0; l < bResp2.results[k].referencesSize; l++) {
                                UA_ReferenceDescription *fr = &bResp2.results[k].references[l];
                                char feName[256];
                                snprintf(feName, sizeof(feName), "%.*s",
                                         (int)fr->browseName.name.length,
                                         fr->browseName.name.data);
                                browseFunctionalEntity(client, fr->nodeId.nodeId, feName);
                            }
                        }
                    }
                    UA_BrowseResponse_clear(&bResp2);
                    UA_BrowseRequest_clear(&bReq2);
                }
            }
        }
    }

    UA_BrowseResponse_clear(&bResp);
    UA_BrowseRequest_clear(&bReq);
}

/* ============================================================
 * Browse struttura UAFX di un singolo server
 * ============================================================ */

static void browseServerUAFX(UA_Client *client, const char *endpoint) {
    printf("\n  ┌─ Endpoint: %s\n", endpoint);

    size_t serverArraySize = 0;
    UA_ApplicationDescription *serverArray = NULL;
    UA_StatusCode retval = UA_Client_findServers(
        client, endpoint, 0, NULL, 0, NULL,
        &serverArraySize, &serverArray);

    if(retval == UA_STATUSCODE_GOOD && serverArraySize > 0) {
        printf("  │  ApplicationUri: %.*s\n",
               (int)serverArray[0].applicationUri.length,
               serverArray[0].applicationUri.data);
        printf("  │  ApplicationName: %.*s\n",
               (int)serverArray[0].applicationName.text.length,
               serverArray[0].applicationName.text.data);
        UA_Array_delete(serverArray, serverArraySize,
                        &UA_TYPES[UA_TYPES_APPLICATIONDESCRIPTION]);
    }

    printf("  │\n");
    printf("  └─ UAFX Structure:\n\n");

    UA_NodeId objectsFolder = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_BrowseRequest bReq;
    UA_BrowseRequest_init(&bReq);
    bReq.requestedMaxReferencesPerNode = 0;
    bReq.nodesToBrowse = UA_BrowseDescription_new();
    bReq.nodesToBrowseSize = 1;
    bReq.nodesToBrowse[0].nodeId = objectsFolder;
    bReq.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bReq.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse bResp = UA_Client_Service_browse(client, bReq);

    if(bResp.responseHeader.serviceResult == UA_STATUSCODE_GOOD) {
        for(size_t i = 0; i < bResp.resultsSize; i++) {
            for(size_t j = 0; j < bResp.results[i].referencesSize; j++) {
                UA_ReferenceDescription *ref = &bResp.results[i].references[j];
                UA_String fxRootStr = UA_STRING("FxRoot");
                if(!UA_String_equal(&ref->browseName.name, &fxRootStr))
                    continue;

                printf("    Objects/FxRoot/\n");

                UA_BrowseRequest bReq2;
                UA_BrowseRequest_init(&bReq2);
                bReq2.requestedMaxReferencesPerNode = 0;
                bReq2.nodesToBrowse = UA_BrowseDescription_new();
                bReq2.nodesToBrowseSize = 1;
                bReq2.nodesToBrowse[0].nodeId = ref->nodeId.nodeId;
                bReq2.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
                bReq2.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

                UA_BrowseResponse bResp2 = UA_Client_Service_browse(client, bReq2);
                if(bResp2.responseHeader.serviceResult == UA_STATUSCODE_GOOD) {
                    for(size_t k = 0; k < bResp2.resultsSize; k++) {
                        for(size_t l = 0; l < bResp2.results[k].referencesSize; l++) {
                            UA_ReferenceDescription *acRef = &bResp2.results[k].references[l];
                            char acName[256];
                            snprintf(acName, sizeof(acName), "%.*s",
                                     (int)acRef->browseName.name.length,
                                     acRef->browseName.name.data);
                            browseAutomationComponent(client, acRef->nodeId.nodeId, acName);
                        }
                    }
                }
                UA_BrowseResponse_clear(&bResp2);
                UA_BrowseRequest_clear(&bReq2);
            }
        }
    }

    UA_BrowseResponse_clear(&bResp);
    UA_BrowseRequest_clear(&bReq);
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(void) {
    printSeparator("OPC UA FX Discovery Client - mDNS via Mini-Server");
    printf("\n");

    /* ─── Phase 1: mDNS Discovery tramite mini-server ─────── */
    printf("[PHASE 1] mDNS Discovery (mini-server integrato)\n");
    printf("──────────────────────────────────────────────────\n");

    DiscoveryList discovered;
    memset(&discovered, 0, sizeof(discovered));

    /*
     * Avvia il mini-server mDNS listener sulla porta 48490
     * (porta arbitraria, non in conflitto con i server UAFX).
     * Aspetta 5 secondi raccogliendo gli annunci mDNS.
     *
     * Regola: più è il timeout, più server possono essere
     * scoperti (i dispositivi annunciano periodicamente).
     * 3-5 secondi è sufficiente per la maggior parte dei casi.
     */
    UA_StatusCode retval = runMdnsDiscovery(&discovered,
                                            /*waitSeconds=*/ 5,
                                            /*listenerPort=*/ 48490);

    if(retval != UA_STATUSCODE_GOOD) {
        printf("⚠ mDNS discovery non disponibile: %s\n",
               UA_StatusCode_name(retval));
        printf("  Usando fallback: connessione diretta a localhost:4840\n\n");
        strncpy(discovered.urls[0], "opc.tcp://localhost:4840",
                MAX_URL_LEN - 1);
        strncpy(discovered.names[0], "localhost (fallback)",
                MAX_URL_LEN - 1);
        discovered.count = 1;
    } else if(discovered.count == 0) {
        printf("\n⚠ Nessun server UAFX trovato via mDNS.\n");
        printf("  Usando fallback: connessione diretta a localhost:4840\n\n");
        strncpy(discovered.urls[0], "opc.tcp://localhost:4840",
                MAX_URL_LEN - 1);
        strncpy(discovered.names[0], "localhost (fallback)",
                MAX_URL_LEN - 1);
        discovered.count = 1;
    } else {
        printf("\n✓ Trovati %zu server via mDNS\n", discovered.count);
    }

    /* ─── Phase 2: Connect & Browse ────────────────────────── */
    printSeparator("PHASE 2: Connect & Browse UAFX Structure");

    UA_Client *client = UA_Client_new();
    UA_ClientConfig_setDefault(UA_Client_getConfig(client));

    for(size_t i = 0; i < discovered.count; i++) {
        printf("\n[Server %zu/%zu] %s\n", i + 1, discovered.count,
               discovered.names[i]);

        retval = UA_Client_connect(client, discovered.urls[i]);

        if(retval != UA_STATUSCODE_GOOD) {
            printf("✗ Connessione fallita: %s\n",
                   UA_StatusCode_name(retval));
            continue;
        }

        printf("✓ Connesso\n");
        browseServerUAFX(client, discovered.urls[i]);
        UA_Client_disconnect(client);
        printf("\n✓ Disconnesso\n");
    }

    UA_Client_delete(client);

    printSeparator("Discovery Completato");
    printf("\n");

    return EXIT_SUCCESS;
}
