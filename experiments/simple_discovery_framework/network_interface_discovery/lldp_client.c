/* ============================================================
 * Client OPC UA FX con mDNS Discovery via Mini-Server Integrato
 *
 * MODIFICATO: Aggiunto supporto per visualizzare NetworkInterfaces
 * e LldpInformation (UAFX Part 82)
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
 * Callback mDNS
 * ============================================================ */

static void
onServerOnNetwork(const UA_ServerOnNetwork *son,
                  UA_Boolean isServerAnnounce,
                  UA_Boolean isTxtReceived,
                  void *data) {

    DiscoveryList *list = (DiscoveryList *)data;

    if(!isServerAnnounce)
        return;

    if(!isTxtReceived)
        return;

    if(list->count >= MAX_DISCOVERED_SERVERS)
        return;

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

    UA_ServerConfig_setMinimal(config, listenerPort, NULL);

    config->mdnsEnabled = UA_TRUE;

    UA_String_clear(&config->mdnsConfig.mdnsServerName);
    config->mdnsConfig.mdnsServerName =
        UA_String_fromChars("UAFX-DiscoveryListener");

    UA_Server_setServerOnNetworkCallback(server, onServerOnNetwork, list);

    UA_StatusCode retval = UA_Server_run_startup(server);
    if(retval != UA_STATUSCODE_GOOD) {
        printf("  Errore avvio mDNS listener: %s\n",
               UA_StatusCode_name(retval));
        UA_Server_delete(server);
        return retval;
    }

    printf("  Ascolto mDNS per %u secondi...\n\n", waitSeconds);

    UA_DateTime stopTime = UA_DateTime_nowMonotonic()
                           + (UA_DateTime)(waitSeconds) * UA_DATETIME_SEC;

    while(UA_DateTime_nowMonotonic() < stopTime) {
        UA_Server_run_iterate(server, false);
        UA_sleep_ms(50);
    }

    UA_Server_run_shutdown(server);
    UA_Server_delete(server);

    return UA_STATUSCODE_GOOD;
#endif
}

/* ============================================================
 * Helper Functions
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

static UA_UInt32 readUInt32Property(UA_Client *client, UA_NodeId parentNode,
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
    UA_UInt32 result = 0;

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
                       UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_UINT32])) {
                        result = *(UA_UInt32*)value.data;
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

static UA_UInt64 readUInt64Property(UA_Client *client, UA_NodeId parentNode,
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
    UA_UInt64 result = 0;

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
                       UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_UINT64])) {
                        result = *(UA_UInt64*)value.data;
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

static void readByteStringProperty(UA_Client *client, UA_NodeId parentNode,
                                    const char *propertyName,
                                    UA_Byte *buffer, size_t bufferSize) {
    UA_BrowseRequest bReq;
    UA_BrowseRequest_init(&bReq);
    bReq.requestedMaxReferencesPerNode = 0;
    bReq.nodesToBrowse = UA_BrowseDescription_new();
    bReq.nodesToBrowseSize = 1;
    bReq.nodesToBrowse[0].nodeId = parentNode;
    bReq.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bReq.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse bResp = UA_Client_Service_browse(client, bReq);

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
                       UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_BYTESTRING])) {
                        UA_ByteString *bs = (UA_ByteString*)value.data;
                        size_t copyLen = bs->length < bufferSize ? bs->length : bufferSize;
                        memcpy(buffer, bs->data, copyLen);
                    }
                    UA_Variant_clear(&value);
                    break;
                }
            }
        }
    }

    UA_BrowseResponse_clear(&bResp);
    UA_BrowseRequest_clear(&bReq);
}

/* ============================================================
 * Browse NetworkInterfaces (UAFX Part 82, 6.5.3)
 * ============================================================ */

static void browseNetworkInterfaces(UA_Client *client, UA_NodeId niNodeId) {
    printf("    │\n");
    printf("    ├─ NetworkInterfaces/ [UAFX Part 82, §6.5.3]\n");

    UA_BrowseRequest bReq;
    UA_BrowseRequest_init(&bReq);
    bReq.requestedMaxReferencesPerNode = 0;
    bReq.nodesToBrowse = UA_BrowseDescription_new();
    bReq.nodesToBrowseSize = 1;
    bReq.nodesToBrowse[0].nodeId = niNodeId;
    bReq.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bReq.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse bResp = UA_Client_Service_browse(client, bReq);

    if(bResp.responseHeader.serviceResult == UA_STATUSCODE_GOOD) {
        for(size_t i = 0; i < bResp.resultsSize; i++) {
            for(size_t j = 0; j < bResp.results[i].referencesSize; j++) {
                UA_ReferenceDescription *ref = &bResp.results[i].references[j];

                printf("    │  └─ %.*s/ [IetfBaseNetworkInterfaceType]\n",
                       (int)ref->browseName.name.length,
                       ref->browseName.name.data);

                /* Leggi proprietà interfaccia */
                UA_UInt32 adminStatus = readUInt32Property(client, ref->nodeId.nodeId, "AdminStatus");
                UA_UInt32 operStatus = readUInt32Property(client, ref->nodeId.nodeId, "OperStatus");
                UA_UInt64 speed = readUInt64Property(client, ref->nodeId.nodeId, "Speed");

                UA_Byte macAddr[6] = {0};
                readByteStringProperty(client, ref->nodeId.nodeId, "PhysAddress", macAddr, 6);

                const char *adminStr = (adminStatus == 1) ? "up" : (adminStatus == 2) ? "down" : "unknown";
                const char *operStr = (operStatus == 1) ? "up" : (operStatus == 2) ? "down" : "unknown";

                printf("    │     ├─ PhysAddress: %02X:%02X:%02X:%02X:%02X:%02X\n",
                       macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
                printf("    │     ├─ AdminStatus: %u (%s)\n", adminStatus, adminStr);
                printf("    │     ├─ OperStatus: %u (%s)\n", operStatus, operStr);
                printf("    │     └─ Speed: %llu bps (%.0f Mbps)\n",
                       (unsigned long long)speed, (double)speed / 1000000.0);
            }
        }
    }

    UA_BrowseResponse_clear(&bResp);
    UA_BrowseRequest_clear(&bReq);
}

/* ============================================================
 * Browse LLDP RemoteSystemsData
 * ============================================================ */

static void browseLldpRemoteSystems(UA_Client *client, UA_NodeId portNodeId) {
    UA_BrowseRequest bReq;
    UA_BrowseRequest_init(&bReq);
    bReq.requestedMaxReferencesPerNode = 0;
    bReq.nodesToBrowse = UA_BrowseDescription_new();
    bReq.nodesToBrowseSize = 1;
    bReq.nodesToBrowse[0].nodeId = portNodeId;
    bReq.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bReq.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse bResp = UA_Client_Service_browse(client, bReq);

    if(bResp.responseHeader.serviceResult == UA_STATUSCODE_GOOD) {
        for(size_t i = 0; i < bResp.resultsSize; i++) {
            for(size_t j = 0; j < bResp.results[i].referencesSize; j++) {
                UA_ReferenceDescription *ref = &bResp.results[i].references[j];
                UA_String remoteDataStr = UA_STRING("RemoteSystemsData");

                if(UA_String_equal(&ref->browseName.name, &remoteDataStr)) {
                    printf("    │           └─ RemoteSystemsData/\n");

                    /* Browse vicini LLDP */
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
                                UA_ReferenceDescription *neighbor = &bResp2.results[k].references[l];

                                printf("    │              ├─ %.*s/\n",
                                       (int)neighbor->browseName.name.length,
                                       neighbor->browseName.name.data);

                                /* Leggi info vicino */
                                UA_Byte remoteMac[6] = {0};
                                readByteStringProperty(client, neighbor->nodeId.nodeId,
                                                      "RemoteChassisId", remoteMac, 6);

                                char *remoteName = readStringProperty(client, neighbor->nodeId.nodeId,
                                                                      "RemoteSystemName");
                                char *remotePort = readStringProperty(client, neighbor->nodeId.nodeId,
                                                                     "RemotePortId");
                                char *remoteMgmt = readStringProperty(client, neighbor->nodeId.nodeId,
                                                                     "RemoteManagementAddress");
                                UA_UInt32 remoteCap = readUInt32Property(client, neighbor->nodeId.nodeId,
                                                                        "RemoteSystemCapabilitiesEnabled");

                                printf("    │              │  ├─ RemoteChassisId: %02X:%02X:%02X:%02X:%02X:%02X\n",
                                       remoteMac[0], remoteMac[1], remoteMac[2],
                                       remoteMac[3], remoteMac[4], remoteMac[5]);

                                if(remoteName) {
                                    printf("    │              │  ├─ RemoteSystemName: %s\n", remoteName);
                                    free(remoteName);
                                }
                                if(remotePort) {
                                    printf("    │              │  ├─ RemotePortId: %s\n", remotePort);
                                    free(remotePort);
                                }
                                if(remoteMgmt) {
                                    printf("    │              │  ├─ RemoteManagementAddress: %s\n", remoteMgmt);
                                    free(remoteMgmt);
                                }

                                printf("    │              │  └─ RemoteCapabilities: 0x%X ", remoteCap);
                                if(remoteCap & 0x04) printf("[Bridge] ");
                                if(remoteCap & 0x10) printf("[Router] ");
                                if(remoteCap & 0x80) printf("[Station] ");
                                if(remoteCap & 0x100) printf("[C-VLAN] ");
                                printf("\n");
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
 * Browse LldpInformation (UAFX Part 82, 6.5.2)
 * ============================================================ */

static void browseLldpInformation(UA_Client *client, UA_NodeId lldpNodeId) {
    printf("    │\n");
    printf("    └─ LldpInformation/ [UAFX Part 82, §6.5.2]\n");

    UA_BrowseRequest bReq;
    UA_BrowseRequest_init(&bReq);
    bReq.requestedMaxReferencesPerNode = 0;
    bReq.nodesToBrowse = UA_BrowseDescription_new();
    bReq.nodesToBrowseSize = 1;
    bReq.nodesToBrowse[0].nodeId = lldpNodeId;
    bReq.nodesToBrowse[0].browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bReq.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse bResp = UA_Client_Service_browse(client, bReq);

    if(bResp.responseHeader.serviceResult == UA_STATUSCODE_GOOD) {
        for(size_t i = 0; i < bResp.resultsSize; i++) {
            for(size_t j = 0; j < bResp.results[i].referencesSize; j++) {
                UA_ReferenceDescription *ref = &bResp.results[i].references[j];

                /* LocalSystemData */
                UA_String localSysStr = UA_STRING("LocalSystemData");
                if(UA_String_equal(&ref->browseName.name, &localSysStr)) {
                    printf("       ├─ LocalSystemData/\n");

                    UA_Byte chassisId[6] = {0};
                    readByteStringProperty(client, ref->nodeId.nodeId, "ChassisId", chassisId, 6);
                    char *sysName = readStringProperty(client, ref->nodeId.nodeId, "SystemName");
                    UA_UInt32 capSupported = readUInt32Property(client, ref->nodeId.nodeId,
                                                                "SystemCapabilitiesSupported");
                    UA_UInt32 capEnabled = readUInt32Property(client, ref->nodeId.nodeId,
                                                              "SystemCapabilitiesEnabled");

                    printf("       │  ├─ ChassisId: %02X:%02X:%02X:%02X:%02X:%02X\n",
                           chassisId[0], chassisId[1], chassisId[2],
                           chassisId[3], chassisId[4], chassisId[5]);

                    if(sysName) {
                        printf("       │  ├─ SystemName: %s\n", sysName);
                        free(sysName);
                    }

                    printf("       │  └─ SystemCapabilities: 0x%X ", capEnabled);
                    if(capEnabled & 0x04) printf("[Bridge] ");
                    if(capEnabled & 0x10) printf("[Router] ");
                    if(capEnabled & 0x80) printf("[Station] ");
                    if(capEnabled & 0x100) printf("[C-VLAN] ");
                    printf("\n");
                }

                /* PortInfo */
                UA_String portInfoStr = UA_STRING("PortInfo");
                if(UA_String_equal(&ref->browseName.name, &portInfoStr)) {
                    printf("       └─ PortInfo/\n");

                    /* Browse porte */
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
                                UA_ReferenceDescription *port = &bResp2.results[k].references[l];

                                printf("          └─ %.*s/\n",
                                       (int)port->browseName.name.length,
                                       port->browseName.name.data);

                                char *portId = readStringProperty(client, port->nodeId.nodeId, "PortId");
                                UA_UInt32 adminStatus = readUInt32Property(client, port->nodeId.nodeId,
                                                                          "AdminStatus");

                                if(portId) {
                                    printf("    │        ├─ PortId: %s\n", portId);
                                    free(portId);
                                }

                                const char *adminStr = (adminStatus == 1) ? "disabled" :
                                                      (adminStatus == 2) ? "enabledTxOnly" :
                                                      (adminStatus == 3) ? "enabledRxOnly" :
                                                      (adminStatus == 4) ? "enabledRxTx" : "unknown";
                                printf("    │        ├─ AdminStatus: %u (%s)\n", adminStatus, adminStr);

                                /* Browse RemoteSystemsData */
                                browseLldpRemoteSystems(client, port->nodeId.nodeId);
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
                    printf("    ├─ FunctionalEntities/\n");

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

                /* ── NetworkInterfaces ── */
                UA_String netIfStr = UA_STRING("NetworkInterfaces");
                if(UA_String_equal(&ref->browseName.name, &netIfStr)) {
                    browseNetworkInterfaces(client, ref->nodeId.nodeId);
                }

                /* ── LldpInformation ── */
                UA_String lldpStr = UA_STRING("LldpInformation");
                if(UA_String_equal(&ref->browseName.name, &lldpStr)) {
                    browseLldpInformation(client, ref->nodeId.nodeId);
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
