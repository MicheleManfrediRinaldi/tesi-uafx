/* ============================================================
 * Client OPC UA FX con:
 * - mDNS Discovery (FindServersOnNetwork)
 * - Connessione ai server scoperti
 * - Browse completo della struttura UAFX
 * - Estrazione metadata FunctionalEntities
 * ============================================================ */

#include "open62541.h"
#include <stdio.h>
#include <string.h>


 //Helper Functions

static void printSeparator(const char *title) {
    printf("\n");
    printf("════════════════════════════════════════════════════\n");
    if(title) {
        printf("  %s\n", title);
        printf("════════════════════════════════════════════════════\n");
    }
}

static void printNodeInfo(const char *label, UA_ReferenceDescription *ref) {
    printf("    %s: %.*s\n", label,
           (int)ref->browseName.name.length,
           ref->browseName.name.data);
}

static char* readStringProperty(UA_Client *client, UA_NodeId parentNode, 
                                const char *propertyName) {
    /* Browse per trovare la property */
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
            UA_BrowseResult *br = &bResp.results[i];
            
            for(size_t j = 0; j < br->referencesSize; j++) {
                UA_ReferenceDescription *ref = &br->references[j];
                
                /* Controlla se è la property cercata */
                UA_String propStr = UA_STRING((char*)propertyName);
                if(UA_String_equal(&ref->browseName.name, &propStr)) {
                    /* Leggi il valore */
                    UA_Variant value;
                    UA_Variant_init(&value);
                    
                    UA_StatusCode retval = UA_Client_readValueAttribute(
                        client, ref->nodeId.nodeId, &value);
                    
                    if(retval == UA_STATUSCODE_GOOD && UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_STRING])) {
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

/* ═══════════════════════════════════════════════════════════
 * Browse FunctionalEntity
 * ═══════════════════════════════════════════════════════════ */

static void browseFunctionalEntity(UA_Client *client, UA_NodeId feNodeId, 
                                   const char *feName) {
    printf("\n      ┌─ FunctionalEntity: %s\n", feName);
    
    /* Leggi metadata della FE */
    char *authorUri = readStringProperty(client, feNodeId, "AuthorUri");
    char *identifier = readStringProperty(client, feNodeId, "AuthorAssignedIdentifier");
    char *version = readStringProperty(client, feNodeId, "AuthorAssignedVersion");
    
    if(authorUri)
        printf("      │  AuthorUri: %s\n", authorUri);
    if(identifier)
        printf("      │  Identifier: %s\n", identifier);
    if(version)
        printf("      │  Version: %s\n", version);
    
    /* Browse children della FE */
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
            UA_BrowseResult *br = &bResp.results[i];
            
            for(size_t j = 0; j < br->referencesSize; j++) {
                UA_ReferenceDescription *ref = &br->references[j];
                
                /* Cerca OutputData folder */
                UA_String outputDataStr = UA_STRING("OutputData");
                if(UA_String_equal(&ref->browseName.name, &outputDataStr)) {
                    printf("      │\n");
                    printf("      └─ OutputData/\n");
                    
                    /* Browse OutputData children */
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
                            UA_BrowseResult *br2 = &bResp2.results[k];
                            
                            for(size_t l = 0; l < br2->referencesSize; l++) {
                                UA_ReferenceDescription *dataRef = &br2->references[l];
                                
                                /* Leggi il valore della variabile */
                                UA_Variant value;
                                UA_Variant_init(&value);
                                
                                UA_StatusCode retval = UA_Client_readValueAttribute(
                                    client, dataRef->nodeId.nodeId, &value);
                                
                                printf("         └─ %.*s",
                                       (int)dataRef->browseName.name.length,
                                       dataRef->browseName.name.data);
                                
                                if(retval == UA_STATUSCODE_GOOD) {
                                    if(UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_FLOAT])) {
                                        UA_Float *floatVal = (UA_Float*)value.data;
                                        printf(": %.2f", *floatVal);
                                        
                                        /* Leggi engineering units */
                                        char *units = readStringProperty(client, dataRef->nodeId.nodeId, 
                                                                        "EngineeringUnits");
                                        if(units) {
                                            printf(" %s", units);
                                            free(units);
                                        }
                                    }
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
    }
    
    UA_BrowseResponse_clear(&bResp);
    UA_BrowseRequest_clear(&bReq);
    
    if(authorUri) free(authorUri);
    if(identifier) free(identifier);
    if(version) free(version);
}

/* ═══════════════════════════════════════════════════════════
 * Browse AutomationComponent
 * ═══════════════════════════════════════════════════════════ */

static void browseAutomationComponent(UA_Client *client, UA_NodeId acNodeId, 
                                      const char *acName) {
    printf("\n    ┌─ AutomationComponent: %s\n", acName);
    
    /* Leggi ConformanceName */
    char *conformanceName = readStringProperty(client, acNodeId, "ConformanceName");
    if(conformanceName) {
        printf("    │  ConformanceName: %s\n", conformanceName);
        free(conformanceName);
    }
    
    /* Browse children dell'AC */
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
            UA_BrowseResult *br = &bResp.results[i];
            
            for(size_t j = 0; j < br->referencesSize; j++) {
                UA_ReferenceDescription *ref = &br->references[j];
                
                /* Cerca Assets folder */
                UA_String assetsStr = UA_STRING("Assets");
                if(UA_String_equal(&ref->browseName.name, &assetsStr)) {
                    printf("    │\n");
                    printf("    ├─ Assets/\n");
                    
                    /* Browse Assets children */
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
                            UA_BrowseResult *br2 = &bResp2.results[k];
                            
                            for(size_t l = 0; l < br2->referencesSize; l++) {
                                UA_ReferenceDescription *assetRef = &br2->references[l];
                                
                                printf("    │  └─ %.*s\n",
                                       (int)assetRef->browseName.name.length,
                                       assetRef->browseName.name.data);
                                
                                /* Leggi alcune properties dell'Asset */
                                char *manufacturer = readStringProperty(client, assetRef->nodeId.nodeId, "Manufacturer");
                                char *model = readStringProperty(client, assetRef->nodeId.nodeId, "Model");
                                char *serial = readStringProperty(client, assetRef->nodeId.nodeId, "SerialNumber");
                                
                                if(manufacturer) {
                                    printf("    │     Manufacturer: %s\n", manufacturer);
                                    free(manufacturer);
                                }
                                if(model) {
                                    printf("    │     Model: %s\n", model);
                                    free(model);
                                }
                                if(serial) {
                                    printf("    │     SerialNumber: %s\n", serial);
                                    free(serial);
                                }
                            }
                        }
                    }
                    
                    UA_BrowseResponse_clear(&bResp2);
                    UA_BrowseRequest_clear(&bReq2);
                }
                
                /* Cerca FunctionalEntities folder */
                UA_String feStr = UA_STRING("FunctionalEntities");
                if(UA_String_equal(&ref->browseName.name, &feStr)) {
                    printf("    │\n");
                    printf("    └─ FunctionalEntities/\n");
                    
                    /* Browse FunctionalEntities children */
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
                            UA_BrowseResult *br2 = &bResp2.results[k];
                            
                            for(size_t l = 0; l < br2->referencesSize; l++) {
                                UA_ReferenceDescription *feRef = &br2->references[l];
                                
                                char feName[256];
                                snprintf(feName, sizeof(feName), "%.*s",
                                        (int)feRef->browseName.name.length,
                                        feRef->browseName.name.data);
                                
                                browseFunctionalEntity(client, feRef->nodeId.nodeId, feName);
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

/* ═══════════════════════════════════════════════════════════
 * Browse Server UAFX Structure
 * ═══════════════════════════════════════════════════════════ */

static void browseServerUAFX(UA_Client *client, const char *endpoint) {
    printf("\n  ┌─ Endpoint: %s\n", endpoint);
    
    /* Leggi ApplicationDescription */
    size_t serverArraySize = 0;
    UA_ApplicationDescription *serverArray = NULL;
    
    UA_StatusCode retval = UA_Client_findServers(client, endpoint, 
                                                  0, NULL, 0, NULL,
                                                  &serverArraySize, &serverArray);
    
    if(retval == UA_STATUSCODE_GOOD && serverArraySize > 0) {
        printf("  │  ApplicationUri: %.*s\n",
               (int)serverArray[0].applicationUri.length,
               serverArray[0].applicationUri.data);
        printf("  │  ApplicationName: %.*s\n",
               (int)serverArray[0].applicationName.text.length,
               serverArray[0].applicationName.text.data);
        
        UA_Array_delete(serverArray, serverArraySize, &UA_TYPES[UA_TYPES_APPLICATIONDESCRIPTION]);
    }
    
    printf("  │\n");
    printf("  └─ UAFX Structure:\n\n");
    
    /* Browse Objects folder */
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
            UA_BrowseResult *result = &bResp.results[i];
            
            for(size_t j = 0; j < result->referencesSize; j++) {
                UA_ReferenceDescription *ref = &result->references[j];
                
                /* Cerca FxRoot */
                UA_String fxRootStr = UA_STRING("FxRoot");
                if(UA_String_equal(&ref->browseName.name, &fxRootStr)) {
                    printf("    Objects/FxRoot/\n");
                    
                    /* Browse FxRoot children */
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
                            UA_BrowseResult *br = &bResp2.results[k];
                            
                            for(size_t l = 0; l < br->referencesSize; l++) {
                                UA_ReferenceDescription *acRef = &br->references[l];
                                
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
    }
    
    UA_BrowseResponse_clear(&bResp);
    UA_BrowseRequest_clear(&bReq);
}

/* ═══════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════ */

int main(void) {
    printSeparator("OPC UA FX Discovery Client");
    printf("\n");
    
    UA_Client *client = UA_Client_new();
    UA_ClientConfig_setDefault(UA_Client_getConfig(client));
    UA_ServerOnNetwork *serverOnNetwork = NULL;
    size_t serverOnNetworkSize = 0;
    UA_StatusCode retval;
    const char *endpoints[10];
    size_t endpointCount = 0;
    
    /* ─── Phase 1: mDNS Discovery ──────────────────────────── */
    printf("[PHASE 1] mDNS Discovery\n");
    printf("──────────────────────────────────────────────────\n");
    
#ifdef UA_ENABLE_DISCOVERY_MULTICAST
    printf("Searching for UAFX servers via mDNS...\n");
    printf("(waiting 3 seconds for responses)\n\n");
    retval = UA_Client_findServersOnNetwork(
        client, "opc.tcp://localhost:4840", 0, 0,
        0, NULL, &serverOnNetworkSize, &serverOnNetwork);
    
    if(retval != UA_STATUSCODE_GOOD) {
        printf("⚠ mDNS discovery failed: %s\n", UA_StatusCode_name(retval));
        printf("  Trying direct connection to localhost:4840...\n\n");
        serverOnNetworkSize = 0;
    } else {
        printf("✓ Found %zu server(s) via mDNS:\n\n", serverOnNetworkSize);
        
        for(size_t i = 0; i < serverOnNetworkSize; i++) {
            printf("  [%zu] %.*s\n", i + 1,
                   (int)serverOnNetwork[i].serverName.length,
                   serverOnNetwork[i].serverName.data);
            printf("      URL: %.*s\n",
                   (int)serverOnNetwork[i].discoveryUrl.length,
                   serverOnNetwork[i].discoveryUrl.data);
            
            if(serverOnNetwork[i].serverCapabilitiesSize > 0) {
                printf("      Capabilities: ");
                for(size_t j = 0; j < serverOnNetwork[i].serverCapabilitiesSize; j++) {
                    printf("%.*s ",
                           (int)serverOnNetwork[i].serverCapabilities[j].length,
                           serverOnNetwork[i].serverCapabilities[j].data);
                }
                printf("\n");
            }
            printf("\n");
        }
    }
#else
    printf("⚠ mDNS discovery NOT compiled in\n");
    printf("  Trying direct connection to localhost:4840...\n\n");
    serverOnNetworkSize = 0;
#endif
    
    /* Se mDNS non trova niente, prova connessione diretta */
    
#ifdef UA_ENABLE_DISCOVERY_MULTICAST
    if(serverOnNetworkSize > 0) {
        for(size_t i = 0; i < serverOnNetworkSize && i < 10; i++) {
            endpoints[endpointCount] = (char*)malloc(serverOnNetwork[i].discoveryUrl.length + 1);
            memcpy((char*)endpoints[endpointCount], 
                   serverOnNetwork[i].discoveryUrl.data, 
                   serverOnNetwork[i].discoveryUrl.length);
            ((char*)endpoints[endpointCount])[serverOnNetwork[i].discoveryUrl.length] = '\0';
            endpointCount++;
        }
        UA_Array_delete(serverOnNetwork, serverOnNetworkSize, 
                       &UA_TYPES[UA_TYPES_SERVERONNETWORK]);
    } else
#endif
    {
        /* Fallback: connessione diretta */
        endpoints[0] = "opc.tcp://localhost:4840";
        endpointCount = 1;
    }
    
    /* ─── Phase 2: Connect & Browse ────────────────────────── */
    printSeparator("PHASE 2: Connect & Browse UAFX Structure");
    
    for(size_t i = 0; i < endpointCount; i++) {
        printf("\n[Server %zu/%zu]\n", i + 1, endpointCount);
        
        retval = UA_Client_connect(client, endpoints[i]);
        
        if(retval != UA_STATUSCODE_GOOD) {
            printf("✗ Connection failed: %s\n", UA_StatusCode_name(retval));
            continue;
        }
        
        printf("✓ Connected\n");
        
        /* Browse UAFX structure */
        browseServerUAFX(client, endpoints[i]);
        
        UA_Client_disconnect(client);
        printf("\n✓ Disconnected\n");
    }
    
    /* Cleanup */
#ifdef UA_ENABLE_DISCOVERY_MULTICAST
    for(size_t i = 0; i < endpointCount && serverOnNetworkSize > 0; i++) {
        free((char*)endpoints[i]);
    }
#endif
    
    UA_Client_delete(client);
    
    printSeparator("Discovery Complete");
    printf("\n");
    
    return EXIT_SUCCESS;
}
