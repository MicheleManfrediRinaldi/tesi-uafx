/* ============================================================
 * uafx_temperature_server.c
 *
 * Server OPC UA FX con tipi UAFX corretti:
 *   - AutomationComponent istanziato come AutomationComponentType (ns=FX/AC; i=2)
 *   - Asset istanziato come FxAssetType                           (ns=FX/AC; i=3)
 *   - FunctionalEntity istanziata come FunctionalEntityType       (ns=FX/AC; i=4)
 *
 * I tipi vengono caricati tramite my_uafx_types() (file generato
 * dal nodeset compiler) prima di costruire l'address space.
 *
 * Compilazione:
 *   gcc -o temp_server temp_serv.c my_uafx_types.c open62541.c -pthread
 * ============================================================ */

#include "open62541.h"
#include "types_di_generated.h"
#include "types_uafx_data_generated.h"
#include "types_uafx_ac_generated.h"
#include "my_uafx_model.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ─── Namespace index di FX/AC nel server ───────────────────────
 * Viene risolto a runtime dopo il caricamento del nodeset.
 * Il file generato registra i namespace in questo ordine:
 *   ns[0] = http://opcfoundation.org/UA/
 *   ns[1] = http://opcfoundation.org/UA/DI/
 *   ns[2] = http://opcfoundation.org/UA/FX/Data/
 *   ns[3] = http://opcfoundation.org/UA/FX/AC/
 * Dopo UA_Server_addNamespace il valore effettivo dipende
 * dall'ordine di registrazione sul server, quindi lo risolviamo
 * leggendo la NamespaceArray. ---------------------------------- */
#define FXAC_NS_URI   "http://opcfoundation.org/UA/FX/AC/"

/* NodeId dei tipi UAFX (numeric id fisso da nodeset XML) */
#define FXAC_ID_AUTOMATIONCOMPONENTTYPE  2
#define FXAC_ID_FXASSETTYPE              3
#define FXAC_ID_FUNCTIONALENTITYTYPE     4

#define NS_LOCAL 1

static volatile UA_Boolean running = true;

static void stopHandler(int sig) {
    printf("\n[SERVER] Shutdown signal received\n");
    running = false;
}

/* ═══════════════════════════════════════════════════════════
 * Risolve il namespace index per una URI data
 * ═══════════════════════════════════════════════════════════ */
static UA_UInt16 resolveNamespaceIndex(UA_Server *server, const char *uri) {
    UA_Variant nsArrayVar;
    UA_Variant_init(&nsArrayVar);

    UA_NodeId nsArrayId = UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_NAMESPACEARRAY);
    UA_StatusCode rc = UA_Server_readValue(server, nsArrayId, &nsArrayVar);
    if(rc != UA_STATUSCODE_GOOD || !UA_Variant_hasArrayType(&nsArrayVar, &UA_TYPES[UA_TYPES_STRING])) {
        UA_Variant_clear(&nsArrayVar);
        printf("[SERVER] WARNING: could not read NamespaceArray, defaulting ns=1\n");
        return 1;
    }

    UA_String *nsArray = (UA_String *)nsArrayVar.data;
    UA_UInt16 found = 0;
    for(size_t i = 0; i < nsArrayVar.arrayLength; i++) {
        UA_String target = UA_STRING((char *)uri);
        if(UA_String_equal(&nsArray[i], &target)) {
            found = (UA_UInt16)i;
            break;
        }
    }

    UA_Variant_clear(&nsArrayVar);
    return found;
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

/* Aggiunge un oggetto con typeDefinition esplicito (UAFX typed) */
static UA_NodeId addTypedObject(UA_Server *server, UA_NodeId parent,
                                UA_UInt16 nsInstance, const char *name,
                                const char *description,
                                UA_UInt16 nsType, UA_UInt32 typeId) {
    UA_ObjectAttributes attr = UA_ObjectAttributes_default;
    attr.displayName = lt(name);
    attr.description = lt(description);

    UA_NodeId newNode = UA_NODEID_NULL;
    UA_Server_addObjectNode(server, UA_NODEID_NULL, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), qn(nsInstance, name),
        UA_NODEID_NUMERIC(nsType, typeId), attr, NULL, &newNode);

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

static void readTemperature(UA_Server *server, const UA_NodeId *sessionId,
                            void *sessionContext, const UA_NodeId *nodeId,
                            void *nodeContext, const UA_NumericRange *range,
                            const UA_DataValue *data) {
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

    UA_ValueCallback callback;
    callback.onRead  = readTemperature;
    callback.onWrite = NULL;
    UA_Server_setVariableNode_valueCallback(server, newNode, callback);

    addStringVariable(server, newNode, ns, "EngineeringUnits", "°C");

    return newNode;
}

/* ═══════════════════════════════════════════════════════════
 * Build UAFX AddressSpace
 *
 * Objects/
 *   └── FxRoot/
 *       └── TemperatureSensor/  [AutomationComponentType  ns=FX/AC; i=2]
 *           ├── ConformanceName
 *           ├── AggregatedHealth
 *           ├── Assets/
 *           │   └── SensorHardware/  [FxAssetType  ns=FX/AC; i=3]
 *           │       ├── Manufacturer, Model, SerialNumber, ...
 *           ├── FunctionalEntities/
 *           │   └── TemperatureReadingFE/  [FunctionalEntityType  ns=FX/AC; i=4]
 *           │       ├── AuthorUri, AuthorAssignedIdentifier, AuthorAssignedVersion
 *           │       ├── OutputData/
 *           │       │   └── Temperature (dynamic)
 *           │       ├── ConnectionEndpoints/
 *           │       └── OperationalHealth
 *           └── ComponentCapabilities/
 * ═══════════════════════════════════════════════════════════ */

static void buildUAFXAddressSpace(UA_Server *server) {
    printf("[SERVER] Building UAFX AddressSpace...\n");

    /* Risolve il namespace index di FX/AC sul server corrente */
    UA_UInt16 nsFxAc = resolveNamespaceIndex(server, FXAC_NS_URI);
    printf("[SERVER]   Namespace FX/AC resolved: %d\n", nsFxAc);

    if(nsFxAc == 0) {
        printf("[SERVER] ERROR: FX/AC namespace not found. "
               "Did you call my_uafx_types() before buildUAFXAddressSpace()?\n");
        return;
    }

    /* ─── 1. FxRoot ──────────────────────────────────────────── */
    UA_NodeId objectsFolder = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId fxRoot = addFolder(server, objectsFolder, nsFxAc, "FxRoot");
    printf("[SERVER]   ✓ FxRoot created\n");

    /* ─── 2. AutomationComponent: TemperatureSensor ──────────── *
     * typeDefinition = AutomationComponentType (ns=FX/AC; i=2)  */
    UA_NodeId acNode = addTypedObject(server, fxRoot,
                                      NS_LOCAL, "TemperatureSensor",
                                      "Temperature Sensor AutomationComponent",
                                      nsFxAc, FXAC_ID_AUTOMATIONCOMPONENTTYPE);
    printf("[SERVER]   ✓ AutomationComponent: TemperatureSensor "
           "[AutomationComponentType ns=%d;i=%d]\n",
           nsFxAc, FXAC_ID_AUTOMATIONCOMPONENTTYPE);

    addStringVariable(server, acNode, NS_LOCAL, "ConformanceName",
                      "urn:example:uafx:temperature-sensor:v1.0");
    addUInt32Variable(server, acNode, NS_LOCAL, "AggregatedHealth", 0);

    /* ─── 3. Assets/ ─────────────────────────────────────────── */
    UA_NodeId assetsFolder = addFolder(server, acNode, NS_LOCAL, "Assets");

    /* typeDefinition = FxAssetType (ns=FX/AC; i=3) */
    UA_NodeId assetNode = addTypedObject(server, assetsFolder,
                                         NS_LOCAL, "SensorHardware",
                                         "Physical temperature sensor hardware",
                                         nsFxAc, FXAC_ID_FXASSETTYPE);
    printf("[SERVER]   ✓ Asset: SensorHardware "
           "[FxAssetType ns=%d;i=%d]\n",
           nsFxAc, FXAC_ID_FXASSETTYPE);

    addStringVariable(server, assetNode, NS_LOCAL, "Manufacturer",      "AcmeCorp");
    addStringVariable(server, assetNode, NS_LOCAL, "ManufacturerUri",   "https://www.acmecorp-sensors.com");
    addStringVariable(server, assetNode, NS_LOCAL, "Model",             "TempSensor-1000");
    addStringVariable(server, assetNode, NS_LOCAL, "ProductCode",       "TS-1000-V2");
    addStringVariable(server, assetNode, NS_LOCAL, "HardwareRevision",  "2.0");
    addStringVariable(server, assetNode, NS_LOCAL, "SoftwareRevision",  "1.3.5");
    addStringVariable(server, assetNode, NS_LOCAL, "DeviceClass",       "TemperatureSensor");
    addStringVariable(server, assetNode, NS_LOCAL, "SerialNumber",      "SN-12345-ABCD");

    /* ─── 4. FunctionalEntities/ ─────────────────────────────── */
    UA_NodeId feFolder = addFolder(server, acNode, NS_LOCAL, "FunctionalEntities");

    /* typeDefinition = FunctionalEntityType (ns=FX/AC; i=4) */
    UA_NodeId feNode = addTypedObject(server, feFolder,
                                      NS_LOCAL, "TemperatureReadingFE",
                                      "Temperature reading functional entity",
                                      nsFxAc, FXAC_ID_FUNCTIONALENTITYTYPE);
    printf("[SERVER]   ✓ FunctionalEntity: TemperatureReadingFE "
           "[FunctionalEntityType ns=%d;i=%d]\n",
           nsFxAc, FXAC_ID_FUNCTIONALENTITYTYPE);

    addStringVariable(server, feNode, NS_LOCAL, "AuthorUri",
                      "https://www.acmecorp-sensors.com");
    addStringVariable(server, feNode, NS_LOCAL, "AuthorAssignedIdentifier",
                      "TempSensor-FE-v1.0");
    addStringVariable(server, feNode, NS_LOCAL, "AuthorAssignedVersion",
                      "1.0.0.0");

    UA_NodeId outputFolder = addFolder(server, feNode, NS_LOCAL, "OutputData");
    addTemperatureVariable(server, outputFolder, NS_LOCAL, "Temperature");
    printf("[SERVER]     └─ OutputData/Temperature (dynamic)\n");

    addFolder(server, feNode, NS_LOCAL, "ConnectionEndpoints");
    addUInt32Variable(server, feNode, NS_LOCAL, "OperationalHealth", 0);

    /* ─── 5. ComponentCapabilities/ ──────────────────────────── */
    UA_NodeId capFolder = addFolder(server, acNode, NS_LOCAL, "ComponentCapabilities");
    addUInt32Variable(server, capFolder, NS_LOCAL, "MaxConnections", 4);
    addUInt32Variable(server, capFolder, NS_LOCAL, "MinConnections", 0);

    printf("[SERVER] ✓ UAFX AddressSpace build complete\n\n");
}

/* ═══════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════ */

int main(void) {
    signal(SIGINT,  stopHandler);
    signal(SIGTERM, stopHandler);
    srand(time(NULL));

    printf("\n");
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║  OPC UA FX Temperature Server                      ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");

    /* ─── Crea server ────────────────────────────────────────── */
    UA_Server *server = UA_Server_new();
    UA_ServerConfig *config = UA_Server_getConfig(server);

    static UA_DataTypeArray customDataTypesAC = {
        NULL, // AC non ha figli in questa catena
        UA_TYPES_UAFX_AC_COUNT,
        UA_TYPES_UAFX_AC
    };

    static UA_DataTypeArray customDataTypesData = {
        &customDataTypesAC, // Colleghiamo AC
        UA_TYPES_UAFX_DATA_COUNT,
        UA_TYPES_UAFX_DATA
    };

    static UA_DataTypeArray customDataTypesDI = {
        &customDataTypesData, // Colleghiamo DATA
        UA_TYPES_DI_COUNT,
        UA_TYPES_DI
    };

    // Diamo in pasto l'intera catena al server
    config->customDataTypes = &customDataTypesDI;
    UA_ServerConfig_setDefault(config);
//    UA_String hostname = UA_String_fromChars("192.168.100.4");
  //  config->customHostname = hostname;
    config->applicationDescription.applicationType = UA_APPLICATIONTYPE_SERVER;

    UA_String_clear(&config->applicationDescription.applicationUri);
    config->applicationDescription.applicationUri =
        UA_String_fromChars("urn:example:uafx:temperature-sensor");

    UA_LocalizedText_clear(&config->applicationDescription.applicationName);
    config->applicationDescription.applicationName =
        UA_LOCALIZEDTEXT_ALLOC("en-US", "UAFX Temperature Sensor");

    config->mdnsEnabled = true;
    config->mdnsConfig.mdnsServerName =
        UA_String_fromChars("MioServer");

    config->mdnsConfig.serverCapabilitiesSize = 1;
    UA_String *caps = (UA_String *)UA_Array_new(1, &UA_TYPES[UA_TYPES_STRING]);
    caps[0] = UA_String_fromChars("UAFX");
    config->mdnsConfig.serverCapabilities = caps;
  //  config->mdnsInterfaceIP = hostname;

#ifdef UA_ENABLE_DISCOVERY_MULTICAST
    printf("[SERVER] ✓ mDNS Discovery: ENABLED\n");
    printf("[SERVER]   Service: _opcua-tcp._tcp.local\n");
    printf("[SERVER]   Capability: UAFX\n\n");
#else
    printf("[SERVER] ⚠ mDNS Discovery: DISABLED\n\n");
#endif

    /* ─── Carica i tipi UAFX dal nodeset generato ────────────── *
     * DEVE essere chiamato prima di buildUAFXAddressSpace()      *
     * perché registra i namespace e i tipi nell'address space.   */
    printf("[SERVER] Loading UAFX nodesets...\n");
    UA_StatusCode retval = my_uafx_model(server);
    if(retval != UA_STATUSCODE_GOOD) {
        printf("[WARNING] Address Space loaded with some missing sub-nodes (Code: %s).\n", UA_StatusCode_name(retval));
        printf("[WARNING] This is normal for massive UAFX NodeSets. Continuing anyway...\n\n");
    } else {
        printf("[SERVER] ✓ UAFX types loaded perfectly\n\n");
    }

    /* ─── Costruisci AddressSpace ────────────────────────────── */
    buildUAFXAddressSpace(server);

    /* ─── Avvia server ───────────────────────────────────────── */
    retval = UA_Server_run_startup(server);
    if(retval != UA_STATUSCODE_GOOD) {
        printf("[ERROR] Server startup failed: %s\n",
               UA_StatusCode_name(retval));
        UA_Server_delete(server);
        return EXIT_FAILURE;
    }

    printf("════════════════════════════════════════════════════\n");
    printf("  SERVER RUNNING\n");
    printf("════════════════════════════════════════════════════\n");
    printf("Endpoint:        opc.tcp://192.168.100.4:4840\n");
    printf("ApplicationUri:  urn:example:uafx:temperature-sensor\n");
    printf("Press Ctrl+C to stop\n");
    printf("════════════════════════════════════════════════════\n\n");

    printf("UAFX Structure:\n");
    printf("  Objects/\n");
    printf("  └── FxRoot/\n");
    printf("      └── TemperatureSensor/ [AutomationComponentType]\n");
    printf("          ├── ConformanceName\n");
    printf("          ├── AggregatedHealth\n");
    printf("          ├── Assets/\n");
    printf("          │   └── SensorHardware/ [FxAssetType]\n");
    printf("          │       ├── Manufacturer: AcmeCorp\n");
    printf("          │       ├── Model: TempSensor-1000\n");
    printf("          │       └── SerialNumber: SN-12345-ABCD\n");
    printf("          ├── FunctionalEntities/\n");
    printf("          │   └── TemperatureReadingFE/ [FunctionalEntityType]\n");
    printf("          │       ├── AuthorUri\n");
    printf("          │       ├── AuthorAssignedIdentifier\n");
    printf("          │       └── OutputData/\n");
    printf("          │           └── Temperature: ~20°C (dynamic)\n");
    printf("          └── ComponentCapabilities/\n\n");

    /* ─── Loop principale ────────────────────────────────────── */
    while(running) {
        UA_Server_run_iterate(server, true);
    }

    printf("\n[SERVER] Shutting down...\n");
    UA_Server_run_shutdown(server);
    UA_Server_delete(server);
    printf("[SERVER] Stopped cleanly\n\n");

    return EXIT_SUCCESS;
}
