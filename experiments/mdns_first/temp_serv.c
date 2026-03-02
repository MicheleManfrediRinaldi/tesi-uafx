/* ============================================================
 * uafx_temperature_server.c
 * 
 * Server OPC UA FX con:
 * - 1 AutomationComponent (TemperatureSensor)
 * - 1 Asset (SensorHardware)
 * - 1 FunctionalEntity (TemperatureReadingFE)
 * - mDNS Discovery abilitato
 * 
 * Compilazione:
 *   gcc -o temp_server uafx_temperature_server.c open62541.c -pthread
 * 
 * Esecuzione:
 *   ./temp_server
 * ============================================================ */

#include "open62541.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NS_LOCAL 1

static volatile UA_Boolean running = true;

static void stopHandler(int sig) {
    printf("\n[SERVER] Shutdown signal received\n");
    running = false;
}

/* ═══════════════════════════════════════════════════════════
 * Helper Functions
 * ═══════════════════════════════════════════════════════════ */

static UA_QualifiedName qn(UA_UInt16 ns, const char *name) {
    return UA_QUALIFIEDNAME(ns, (char *)name);
}

static UA_LocalizedText lt(const char *text) {
    return UA_LOCALIZEDTEXT("en-US", (char *)text);
}

static UA_NodeId addFolder(UA_Server *server, UA_NodeId parent, 
                            UA_UInt16 ns, const char *name) {
    UA_ObjectAttributes attr = UA_ObjectAttributes_default;
    attr.displayName = lt(name);
    
    UA_NodeId newNode = UA_NODEID_NULL;
    UA_Server_addObjectNode(server, UA_NODEID_NULL, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), qn(ns, name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE), attr, NULL, &newNode);
    
    return newNode;
}

static UA_NodeId addObject(UA_Server *server, UA_NodeId parent, 
                            UA_UInt16 ns, const char *name, const char *description) {
    UA_ObjectAttributes attr = UA_ObjectAttributes_default;
    attr.displayName = lt(name);
    attr.description = lt(description);
    
    UA_NodeId newNode = UA_NODEID_NULL;
    UA_Server_addObjectNode(server, UA_NODEID_NULL, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), qn(ns, name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), attr, NULL, &newNode);
    
    return newNode;
}

static UA_NodeId addStringVariable(UA_Server *server, UA_NodeId parent,
                                    UA_UInt16 ns, const char *name, const char *value) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = lt(name);
    UA_String val = UA_STRING((char *)value);
    UA_Variant_setScalar(&attr.value, &val, &UA_TYPES[UA_TYPES_STRING]);
    attr.dataType = UA_TYPES[UA_TYPES_STRING].typeId;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    
    UA_NodeId newNode = UA_NODEID_NULL;
    UA_Server_addVariableNode(server, UA_NODEID_NULL, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY), qn(ns, name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), attr, NULL, &newNode);
    
    return newNode;
}

static UA_NodeId addUInt32Variable(UA_Server *server, UA_NodeId parent,
                                    UA_UInt16 ns, const char *name, UA_UInt32 value) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = lt(name);
    UA_Variant_setScalar(&attr.value, &value, &UA_TYPES[UA_TYPES_UINT32]);
    attr.dataType = UA_TYPES[UA_TYPES_UINT32].typeId;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    
    UA_NodeId newNode = UA_NODEID_NULL;
    UA_Server_addVariableNode(server, UA_NODEID_NULL, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), qn(ns, name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, NULL, &newNode);
    
    return newNode;
}

/* ═══════════════════════════════════════════════════════════
 * Temperature Variable with Dynamic Callback
 * ═══════════════════════════════════════════════════════════ */

static void readTemperature(UA_Server *server, const UA_NodeId *sessionId, void *sessionContext,
                            const UA_NodeId *nodeId, void *nodeContext,
                            const UA_NumericRange *range, const UA_DataValue *data) {
    /* Simula lettura temperatura: 20°C ± 5°C */
    UA_Float temperature = 20.0f + ((rand() % 1000) - 500) / 100.0f;
    
    UA_Variant value;
    UA_Variant_setScalar(&value, &temperature, &UA_TYPES[UA_TYPES_FLOAT]);
    UA_Server_writeValue(server, *nodeId, value);
}

static UA_NodeId addTemperatureVariable(UA_Server *server, UA_NodeId parent,
                                        UA_UInt16 ns, const char *name) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = lt(name);
    attr.description = lt("Current temperature reading in degrees Celsius");
    
    UA_Float initialValue = 20.0f;
    UA_Variant_setScalar(&attr.value, &initialValue, &UA_TYPES[UA_TYPES_FLOAT]);
    attr.dataType = UA_TYPES[UA_TYPES_FLOAT].typeId;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;
    
    UA_NodeId newNode = UA_NODEID_NULL;
    UA_Server_addVariableNode(server, UA_NODEID_NULL, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), qn(ns, name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, NULL, &newNode);
    
    /* Registra callback per simulare lettura dinamica */
    UA_ValueCallback callback;
    callback.onRead = readTemperature;
    callback.onWrite = NULL;
    UA_Server_setVariableNode_valueCallback(server, newNode, callback);
    
    /* Aggiungi EngineeringUnits */
    addStringVariable(server, newNode, ns, "EngineeringUnits", "°C");
    
    return newNode;
}

/* ═══════════════════════════════════════════════════════════
 * Build UAFX AddressSpace
 * 
 * Struttura:
 * Objects/
 *   └── FxRoot/
 *       └── TemperatureSensor/ [AutomationComponent]
 *           ├── ConformanceName
 *           ├── AggregatedHealth
 *           ├── Assets/
 *           │   └── SensorHardware/ [Asset]
 *           │       ├── Manufacturer
 *           │       ├── Model
 *           │       ├── SerialNumber
 *           │       └── ... (altre properties)
 *           ├── FunctionalEntities/
 *           │   └── TemperatureReadingFE/ [FunctionalEntity]
 *           │       ├── AuthorUri
 *           │       ├── AuthorAssignedIdentifier
 *           │       ├── AuthorAssignedVersion
 *           │       ├── OutputData/
 *           │       │   └── Temperature (dynamic)
 *           │       ├── ConnectionEndpoints/ (empty)
 *           │       └── OperationalHealth
 *           ├── ComponentCapabilities/
 *           │   ├── MaxConnections
 *           │   └── SupportsPersistence
 *           └── Descriptors/
 *               └── ProductDescriptor/
 * ═══════════════════════════════════════════════════════════ */

static void buildUAFXAddressSpace(UA_Server *server) {
    printf("[SERVER] Building UAFX AddressSpace...\n");
    
    /* Registra namespace UAFX */
    UA_UInt16 nsFxAc = UA_Server_addNamespace(server, 
        "http://opcfoundation.org/UA/FX/AC/");
    
    printf("[SERVER]   Namespace FX/AC registered: %d\n", nsFxAc);
    
    /* ─── 1. FxRoot (entry point UAFX) ───────────────────── */
    UA_NodeId objectsFolder = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId fxRoot = addFolder(server, objectsFolder, nsFxAc, "FxRoot");
    printf("[SERVER]   ✓ FxRoot created\n");
    
    /* ─── 2. AutomationComponent: TemperatureSensor ──────── */
    UA_NodeId acNode = addObject(server, fxRoot, NS_LOCAL, "TemperatureSensor",
                                  "Temperature Sensor AutomationComponent");
    printf("[SERVER]   ✓ AutomationComponent: TemperatureSensor\n");
    
    /* ConformanceName - identifica il tipo di componente */
    addStringVariable(server, acNode, NS_LOCAL, "ConformanceName",
                     "urn:example:uafx:temperature-sensor:v1.0");
    
    /* AggregatedHealth - stato di salute aggregato */
    addUInt32Variable(server, acNode, NS_LOCAL, "AggregatedHealth", 0);
    
    /* ─── 3. Assets/ ─────────────────────────────────────── */
    UA_NodeId assetsFolder = addFolder(server, acNode, NS_LOCAL, "Assets");
    
    UA_NodeId assetNode = addObject(server, assetsFolder, NS_LOCAL, "SensorHardware",
                                     "Physical temperature sensor hardware");
    
    /* IVendorNameplateType properties */
    addStringVariable(server, assetNode, NS_LOCAL, "Manufacturer", "AcmeCorp");
    addStringVariable(server, assetNode, NS_LOCAL, "ManufacturerUri", 
                     "https://www.acmecorp-sensors.com");
    addStringVariable(server, assetNode, NS_LOCAL, "Model", "TempSensor-1000");
    addStringVariable(server, assetNode, NS_LOCAL, "ProductCode", "TS-1000-V2");
    addStringVariable(server, assetNode, NS_LOCAL, "HardwareRevision", "2.0");
    addStringVariable(server, assetNode, NS_LOCAL, "SoftwareRevision", "1.3.5");
    addStringVariable(server, assetNode, NS_LOCAL, "DeviceClass", "TemperatureSensor");
    addStringVariable(server, assetNode, NS_LOCAL, "SerialNumber", "SN-12345-ABCD");
    
    printf("[SERVER]   ✓ Asset: SensorHardware\n");
    
    /* ─── 4. FunctionalEntities/ ─────────────────────────── */
    UA_NodeId feFolder = addFolder(server, acNode, NS_LOCAL, "FunctionalEntities");
    
    UA_NodeId feNode = addObject(server, feFolder, NS_LOCAL, "TemperatureReadingFE",
                                  "Temperature reading functional entity");
    
    /* FE identification properties */
    addStringVariable(server, feNode, NS_LOCAL, "AuthorUri",
                     "https://www.acmecorp-sensors.com");
    addStringVariable(server, feNode, NS_LOCAL, "AuthorAssignedIdentifier",
                     "TempSensor-FE-v1.0");
    addStringVariable(server, feNode, NS_LOCAL, "AuthorAssignedVersion",
                     "1.0.0.0");
    
    /* OutputData/ - dati prodotti dalla FE */
    UA_NodeId outputFolder = addFolder(server, feNode, NS_LOCAL, "OutputData");
    addTemperatureVariable(server, outputFolder, NS_LOCAL, "Temperature");
    
    /* ConnectionEndpoints/ - per PubSub (vuoto per ora) */
    addFolder(server, feNode, NS_LOCAL, "ConnectionEndpoints");
    
    /* OperationalHealth - stato di salute della FE */
    addUInt32Variable(server, feNode, NS_LOCAL, "OperationalHealth", 0);
    
    printf("[SERVER]   ✓ FunctionalEntity: TemperatureReadingFE\n");
    printf("[SERVER]     └─ OutputData/Temperature (dynamic)\n");
    
    /* ─── 5. ComponentCapabilities/ ──────────────────────── */
    UA_NodeId capFolder = addFolder(server, acNode, NS_LOCAL, "ComponentCapabilities");
    addUInt32Variable(server, capFolder, NS_LOCAL, "MaxConnections", 4);
    addUInt32Variable(server, capFolder, NS_LOCAL, "MinConnections", 0);
    
    /* ─── 6. Descriptors/ ────────────────────────────────── */
    UA_NodeId descFolder = addFolder(server, acNode, NS_LOCAL, "Descriptors");
    UA_NodeId descNode = addObject(server, descFolder, NS_LOCAL, "ProductDescriptor",
                                    "UAFX Product Descriptor");
    addStringVariable(server, descNode, NS_LOCAL, "DescriptorIdentifier",
                     "urn:acmecorp:uafx:descriptor:temp-sensor:1.0.0");
    
    printf("[SERVER] ✓ UAFX AddressSpace build complete\n\n");
}

/* ═══════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════ */

int main(void) {
    signal(SIGINT, stopHandler);
    signal(SIGTERM, stopHandler);
    srand(time(NULL));
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║  OPC UA FX Temperature Server                      ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");
    
    /* Crea server */
    UA_Server *server = UA_Server_new();
    UA_ServerConfig *config = UA_Server_getConfig(server);
    UA_ServerConfig_setDefault(config);
    
    /* Configurazione ApplicationDescription */
    config->applicationDescription.applicationType = UA_APPLICATIONTYPE_SERVER;
    
    UA_String_clear(&config->applicationDescription.applicationUri);
    config->applicationDescription.applicationUri = 
        UA_String_fromChars("urn:example:uafx:temperature-sensor");
    
    UA_LocalizedText_clear(&config->applicationDescription.applicationName);
    config->applicationDescription.applicationName = 
        UA_LOCALIZEDTEXT_ALLOC("en-US", "UAFX Temperature Sensor");
    config->mdnsEnabled = true; // (oppure config->discovery.mdnsEnable = true; a seconda della versione della libreria)
    config->mdnsConfig.mdnsServerName = UA_String_fromChars("TemperatureSensorUAFX");
    
    /* Dichiara le capacità UAFX (Opzionale ma molto utile per il client) */
    config->mdnsConfig.serverCapabilitiesSize = 1;
    UA_String *caps = (UA_String *) UA_Array_new(1, &UA_TYPES[UA_TYPES_STRING]);
    caps[0] = UA_String_fromChars("UAFX");
    config->mdnsConfig.serverCapabilities = caps;
    /* ========================================== */
    
    /* Verifica mDNS */
#ifdef UA_ENABLE_DISCOVERY_MULTICAST
    printf("[SERVER] ✓ mDNS Discovery: ENABLED\n");
    printf("[SERVER]   Service: _opcua-tcp._tcp.local\n");
    printf("[SERVER]   Capability: UAFX\n\n");
#else
    printf("[SERVER] ⚠ mDNS Discovery: DISABLED\n");
    printf("[SERVER]   (Server still accessible via direct connection)\n\n");
#endif
    
    /* Costruisci AddressSpace UAFX */
    buildUAFXAddressSpace(server);
    
    /* Avvia server */
    UA_StatusCode retval = UA_Server_run_startup(server);
    if(retval != UA_STATUSCODE_GOOD) {
        printf("[ERROR] Server startup failed: %s\n", UA_StatusCode_name(retval));
        UA_Server_delete(server);
        return EXIT_FAILURE;
    }
 	
    
    printf("════════════════════════════════════════════════════\n");
    printf("  SERVER RUNNING\n");
    printf("════════════════════════════════════════════════════\n");
    printf("Endpoint:        opc.tcp://localhost:4840\n");
    printf("ApplicationUri:  urn:example:uafx:temperature-sensor\n");
    printf("Press Ctrl+C to stop\n");
    printf("════════════════════════════════════════════════════\n\n");
    
    printf("UAFX Structure:\n");
    printf("  Objects/\n");
    printf("  └── FxRoot/\n");
    printf("      └── TemperatureSensor/ [AutomationComponent]\n");
    printf("          ├── ConformanceName: urn:example:uafx:temperature-sensor:v1.0\n");
    printf("          ├── AggregatedHealth: 0\n");
    printf("          ├── Assets/\n");
    printf("          │   └── SensorHardware/\n");
    printf("          │       ├── Manufacturer: AcmeCorp\n");
    printf("          │       ├── Model: TempSensor-1000\n");
    printf("          │       └── SerialNumber: SN-12345-ABCD\n");
    printf("          ├── FunctionalEntities/\n");
    printf("          │   └── TemperatureReadingFE/\n");
    printf("          │       ├── AuthorUri: https://www.acmecorp-sensors.com\n");
    printf("          │       ├── AuthorAssignedIdentifier: TempSensor-FE-v1.0\n");
    printf("          │       └── OutputData/\n");
    printf("          │           └── Temperature: ~20°C (dynamic)\n");
    printf("          └── ComponentCapabilities/\n");
    printf("              └── MaxConnections: 4\n\n");
    
    /* Loop principale */
    while(running) {
        UA_Server_run_iterate(server, true);
    }
    
    /* Shutdown */
    printf("\n[SERVER] Shutting down...\n");
    UA_Server_run_shutdown(server);
    UA_Server_delete(server);
    printf("[SERVER] Stopped cleanly\n\n");
    
    return EXIT_SUCCESS;
}
