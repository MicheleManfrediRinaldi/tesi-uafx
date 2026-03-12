
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include "open62541.h"
#define NS_LOCAL 1
#define MAX_NEIGHBORS 10
#define MAX_INTERFACES 10

static volatile UA_Boolean running = true;

/* ═══════════════════════════════════════════════════════════
 * Strutture Dati per Discovery Dinamica
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    char name[32];
    UA_Byte macAddr[6];
    UA_Boolean isUp;
    UA_UInt64 speed;
} NetworkInterface;

typedef struct {
    UA_Byte chassisId[6];
    UA_UInt32 chassisIdSubtype;
    char portId[64];
    UA_UInt32 portIdSubtype;
    char systemName[128];
    char systemDescription[256];
    char portDescription[128];
    char managementAddress[64];
    UA_UInt32 managementAddressSubtype;
    UA_UInt32 capabilitiesSupported;
    UA_UInt32 capabilitiesEnabled;
    UA_UInt32 timeToLive;
    UA_Boolean valid;
} LldpNeighbor;

typedef struct {
    NetworkInterface interfaces[MAX_INTERFACES];
    int interfaceCount;
    LldpNeighbor neighbors[MAX_NEIGHBORS];
    int neighborCount;
    UA_Boolean discoverySuccess;
} TopologyData;

static void stopHandler(int sig) {
    printf("\n[SERVER] Shutdown signal received\n");
    running = false;
}

/* ═══════════════════════════════════════════════════════════
 * Topology Discovery Functions
 * ═══════════════════════════════════════════════════════════ */

/* Legge il MAC address di un'interfaccia */
static UA_Boolean readMacAddress(const char *ifname, UA_Byte *macAddr) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return UA_FALSE;

    struct ifreq ifr;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = 0;

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        close(sock);
        return UA_FALSE;
    }

    memcpy(macAddr, ifr.ifr_hwaddr.sa_data, 6);
    close(sock);
    return UA_TRUE;
}

/* Legge lo stato operativo di un'interfaccia */
static UA_Boolean isInterfaceUp(const char *ifname) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return UA_FALSE;

    struct ifreq ifr;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = 0;

    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        close(sock);
        return UA_FALSE;
    }

    close(sock);
    return (ifr.ifr_flags & IFF_UP) != 0;
}

/* Legge la velocità dell'interfaccia da /sys */
static UA_UInt64 readInterfaceSpeed(const char *ifname) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/net/%s/speed", ifname);

    FILE *f = fopen(path, "r");
    if (!f) return 1000000000ULL; // Default 1 Gbps

    int speedMbps = 0;
    if (fscanf(f, "%d", &speedMbps) != 1) {
        fclose(f);
        return 1000000000ULL;
    }

    fclose(f);
    return (UA_UInt64)speedMbps * 1000000ULL; // Converti Mbps in bps
}

/* Scopre le interfacce di rete reali dal sistema */
static void discoverNetworkInterfaces(TopologyData *topology) {
    printf("[DISCOVERY] Discovering network interfaces...\n");

    topology->interfaceCount = 0;

    DIR *dir = opendir("/sys/class/net");
    if (!dir) {
        printf("[DISCOVERY] ⚠ Cannot open /sys/class/net\n");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && topology->interfaceCount < MAX_INTERFACES) {
        if (entry->d_name[0] == '.') continue;
        if (strcmp(entry->d_name, "lo") == 0) continue; // Skip loopback

        NetworkInterface *iface = &topology->interfaces[topology->interfaceCount];
        strncpy(iface->name, entry->d_name, sizeof(iface->name) - 1);
        iface->name[sizeof(iface->name) - 1] = 0;

        iface->isUp = isInterfaceUp(iface->name);
        iface->speed = readInterfaceSpeed(iface->name);

        if (readMacAddress(iface->name, iface->macAddr)) {
            printf("[DISCOVERY]   ✓ Found interface: %s (MAC: %02X:%02X:%02X:%02X:%02X:%02X, %s)\n",
                   iface->name,
                   iface->macAddr[0], iface->macAddr[1], iface->macAddr[2],
                   iface->macAddr[3], iface->macAddr[4], iface->macAddr[5],
                   iface->isUp ? "UP" : "DOWN");
            topology->interfaceCount++;
        }
    }

    closedir(dir);

    if (topology->interfaceCount == 0) {
        printf("[DISCOVERY] ⚠ No physical interfaces found\n");
    }
}

/* Tenta di leggere i vicini LLDP da lldpctl (se disponibile) */
static void discoverLldpNeighbors(TopologyData *topology) {
    printf("[DISCOVERY] Attempting LLDP neighbor discovery...\n");

    topology->neighborCount = 0;

    /* Prova a eseguire lldpctl per ottenere informazioni LLDP */
    FILE *fp = popen("lldpctl -f keyvalue 2>/dev/null", "r");
    if (!fp) {
        printf("[DISCOVERY] ⚠ lldpctl not available (install lldpd for dynamic discovery)\n");
        return;
    }

    char line[512];
    LldpNeighbor *currentNeighbor = NULL;

    while (fgets(line, sizeof(line), fp) != NULL && topology->neighborCount < MAX_NEIGHBORS) {
        /* Rimuovi newline */
        line[strcspn(line, "\n")] = 0;

        /* Parsing semplificato dell'output lldpctl in formato keyvalue */
        if (strstr(line, "lldp.") == line) {
            char *key = line;
            char *value = strchr(line, '=');
            if (!value) continue;
            *value = 0;
            value++;

            /* Nuovo vicino */
            if (strstr(key, ".chassis.mac")) {
                if (currentNeighbor && currentNeighbor->valid) {
                    topology->neighborCount++;
                }
                if (topology->neighborCount < MAX_NEIGHBORS) {
                    currentNeighbor = &topology->neighbors[topology->neighborCount];
                    memset(currentNeighbor, 0, sizeof(LldpNeighbor));
                    currentNeighbor->chassisIdSubtype = 4; // MAC address
                    currentNeighbor->portIdSubtype = 5; // Interface name
                    currentNeighbor->managementAddressSubtype = 1; // IPv4
                    currentNeighbor->timeToLive = 120;
                    currentNeighbor->valid = UA_TRUE;

                    /* Parse MAC address */
                    sscanf(value, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                           &currentNeighbor->chassisId[0], &currentNeighbor->chassisId[1],
                           &currentNeighbor->chassisId[2], &currentNeighbor->chassisId[3],
                           &currentNeighbor->chassisId[4], &currentNeighbor->chassisId[5]);
                }
            }
            else if (currentNeighbor && strstr(key, ".port.ifname")) {
                strncpy(currentNeighbor->portId, value, sizeof(currentNeighbor->portId) - 1);
            }
            else if (currentNeighbor && strstr(key, ".chassis.name")) {
                strncpy(currentNeighbor->systemName, value, sizeof(currentNeighbor->systemName) - 1);
            }
            else if (currentNeighbor && strstr(key, ".chassis.descr")) {
                strncpy(currentNeighbor->systemDescription, value, sizeof(currentNeighbor->systemDescription) - 1);
            }
            else if (currentNeighbor && strstr(key, ".port.descr")) {
                strncpy(currentNeighbor->portDescription, value, sizeof(currentNeighbor->portDescription) - 1);
            }
            else if (currentNeighbor && strstr(key, ".chassis.mgmt-ip")) {
                strncpy(currentNeighbor->managementAddress, value, sizeof(currentNeighbor->managementAddress) - 1);
            }
            else if (currentNeighbor && strstr(key, ".chassis.capability")) {
                if (strstr(value, "Bridge")) currentNeighbor->capabilitiesEnabled |= 0x04;
                if (strstr(value, "Router")) currentNeighbor->capabilitiesEnabled |= 0x10;
                if (strstr(value, "Station")) currentNeighbor->capabilitiesEnabled |= 0x80;
            }
        }
    }

    /* Aggiungi l'ultimo vicino se valido */
    if (currentNeighbor && currentNeighbor->valid) {
        topology->neighborCount++;
    }

    pclose(fp);

    if (topology->neighborCount > 0) {
        printf("[DISCOVERY] ✓ Found %d LLDP neighbor(s)\n", topology->neighborCount);
        for (int i = 0; i < topology->neighborCount; i++) {
            LldpNeighbor *n = &topology->neighbors[i];
            printf("[DISCOVERY]   - %s (MAC: %02X:%02X:%02X:%02X:%02X:%02X, Port: %s)\n",
                   n->systemName[0] ? n->systemName : "Unknown",
                   n->chassisId[0], n->chassisId[1], n->chassisId[2],
                   n->chassisId[3], n->chassisId[4], n->chassisId[5],
                   n->portId[0] ? n->portId : "Unknown");
        }
    } else {
        printf("[DISCOVERY] ⚠ No LLDP neighbors found\n");
    }
}

/* Funzione principale di topology discovery */
static void performTopologyDiscovery(TopologyData *topology) {
    memset(topology, 0, sizeof(TopologyData));

    printf("\n");
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║  TOPOLOGY DISCOVERY                                ║\n");
    printf("╚════════════════════════════════════════════════════╝\n");

    discoverNetworkInterfaces(topology);
    discoverLldpNeighbors(topology);

    topology->discoverySuccess = (topology->interfaceCount > 0);

    if (topology->discoverySuccess) {
        printf("\n[DISCOVERY] ✓ Discovery completed successfully\n");
    } else {
        printf("\n[DISCOVERY] ⚠ Discovery failed - using static fallback data\n");
    }
    printf("\n");
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

static UA_NodeId addByteStringVariable(UA_Server *server, UA_NodeId parent,
                                        UA_UInt16 ns, const char *name,
                                        const UA_Byte *data, size_t length) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = lt(name);
    UA_ByteString val = {length, (UA_Byte *)data};
    UA_Variant_setScalar(&attr.value, &val, &UA_TYPES[UA_TYPES_BYTESTRING]);
    attr.dataType = UA_TYPES[UA_TYPES_BYTESTRING].typeId;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;

    UA_NodeId newNode = UA_NODEID_NULL;
    UA_Server_addVariableNode(server, UA_NODEID_NULL, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), qn(ns, name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, NULL, &newNode);

    return newNode;
}

static UA_NodeId addUInt64Variable(UA_Server *server, UA_NodeId parent,
                                    UA_UInt16 ns, const char *name, UA_UInt64 value) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = lt(name);
    UA_Variant_setScalar(&attr.value, &value, &UA_TYPES[UA_TYPES_UINT64]);
    attr.dataType = UA_TYPES[UA_TYPES_UINT64].typeId;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ;

    UA_NodeId newNode = UA_NODEID_NULL;
    UA_Server_addVariableNode(server, UA_NODEID_NULL, parent,
        UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), qn(ns, name),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, NULL, &newNode);

    return newNode;
}

static UA_NodeId addBooleanVariable(UA_Server *server, UA_NodeId parent,
                                     UA_UInt16 ns, const char *name, UA_Boolean value) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = lt(name);
    UA_Variant_setScalar(&attr.value, &value, &UA_TYPES[UA_TYPES_BOOLEAN]);
    attr.dataType = UA_TYPES[UA_TYPES_BOOLEAN].typeId;
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
 * UAFX Network Interfaces (Part 82, 6.5.3)
 * IetfBaseNetworkInterfaceType per rappresentare interfacce fisiche
 * ═══════════════════════════════════════════════════════════ */

static void buildNetworkInterfaces(UA_Server *server, UA_NodeId acNode, UA_UInt16 ns, TopologyData *topology) {
    printf("[SERVER] Building NetworkInterfaces...\n");

    /* NetworkInterfaces folder */
    UA_NodeId netIfFolder = addFolder(server, acNode, ns, "NetworkInterfaces");

    if (topology->interfaceCount > 0) {
        /* Usa le interfacce scoperte dinamicamente */
        for (int i = 0; i < topology->interfaceCount; i++) {
            NetworkInterface *iface = &topology->interfaces[i];

            char desc[128];
            snprintf(desc, sizeof(desc), "Network interface %s", iface->name);
            UA_NodeId ifNode = addObject(server, netIfFolder, ns, iface->name, desc);

            /* AdminStatus: 1=up, 2=down */
            addUInt32Variable(server, ifNode, ns, "AdminStatus", iface->isUp ? 1 : 2);

            /* OperStatus */
            addUInt32Variable(server, ifNode, ns, "OperStatus", iface->isUp ? 1 : 2);

            /* PhysAddress: MAC address */
            addByteStringVariable(server, ifNode, ns, "PhysAddress", iface->macAddr, 6);

            /* Speed */
            addUInt64Variable(server, ifNode, ns, "Speed", iface->speed);

            /* Name */
            addStringVariable(server, ifNode, ns, "Name", iface->name);

            /* Type: 6=ethernetCsmacd */
            addUInt32Variable(server, ifNode, ns, "Type", 6);

            printf("[SERVER]   ✓ NetworkInterface: %s (MAC: %02X:%02X:%02X:%02X:%02X:%02X)\n",
                   iface->name,
                   iface->macAddr[0], iface->macAddr[1], iface->macAddr[2],
                   iface->macAddr[3], iface->macAddr[4], iface->macAddr[5]);
        }
    } else {
        /* Fallback: interfaccia statica per testing */
        printf("[SERVER]   Using static fallback interface\n");

        UA_NodeId eth0 = addObject(server, netIfFolder, ns, "eth0",
                                    "Primary physical network interface (static)");

        addUInt32Variable(server, eth0, ns, "AdminStatus", 1);
        addUInt32Variable(server, eth0, ns, "OperStatus", 1);

        UA_Byte macAddr[6] = {0x00, 0x1B, 0x44, 0x11, 0x3A, 0xB7};
        addByteStringVariable(server, eth0, ns, "PhysAddress", macAddr, 6);

        addUInt64Variable(server, eth0, ns, "Speed", 1000000000ULL);
        addStringVariable(server, eth0, ns, "Name", "eth0");
        addUInt32Variable(server, eth0, ns, "Type", 6);

        printf("[SERVER]   ✓ NetworkInterface: eth0 (MAC: 00:1B:44:11:3A:B7) [STATIC]\n");
    }
}

/* ═══════════════════════════════════════════════════════════
 * LLDP Information (Part 82, 6.5.2)
 * Rappresentazione delle informazioni LLDP secondo IEEE 802.1AB
 * ═══════════════════════════════════════════════════════════ */

static void buildLldpInformation(UA_Server *server, UA_NodeId acNode, UA_UInt16 ns, TopologyData *topology) {
    printf("[SERVER] Building LLDP Information...\n");

    UA_NodeId lldpInfo = addObject(server, acNode, ns, "LldpInformation",
                                    "LLDP topology discovery information");

    /* ─────────────────────────────────────────────────────────
     * LocalSystemData: usa la prima interfaccia scoperta o fallback
     * ───────────────────────────────────────────────────────── */
    UA_NodeId localSys = addFolder(server, lldpInfo, ns, "LocalSystemData");

    UA_Byte *chassisId;
    if (topology->interfaceCount > 0) {
        chassisId = topology->interfaces[0].macAddr;
    } else {
        static UA_Byte fallbackMac[6] = {0x00, 0x1B, 0x44, 0x11, 0x3A, 0xB7};
        chassisId = fallbackMac;
    }

    addByteStringVariable(server, localSys, ns, "ChassisId", chassisId, 6);
    addUInt32Variable(server, localSys, ns, "ChassisIdSubtype", 4);
    addStringVariable(server, localSys, ns, "SystemName", "TemperatureSensorUAFX");
    addStringVariable(server, localSys, ns, "SystemDescription",
                     "UAFX Temperature Sensor - AcmeCorp TempSensor-1000");
    addUInt32Variable(server, localSys, ns, "SystemCapabilitiesSupported", 0x80);
    addUInt32Variable(server, localSys, ns, "SystemCapabilitiesEnabled", 0x80);

    printf("[SERVER]   ✓ LocalSystemData created\n");

    /* ─────────────────────────────────────────────────────────
     * PortInfo: crea info per ogni interfaccia scoperta
     * ───────────────────────────────────────────────────────── */
    UA_NodeId portInfo = addFolder(server, lldpInfo, ns, "PortInfo");

    int interfacesToProcess = (topology->interfaceCount > 0) ? topology->interfaceCount : 1;

    for (int i = 0; i < interfacesToProcess; i++) {
        const char *ifname;
        if (topology->interfaceCount > 0) {
            ifname = topology->interfaces[i].name;
        } else {
            ifname = "eth0";
        }

        char desc[128];
        snprintf(desc, sizeof(desc), "LLDP port information for %s", ifname);
        UA_NodeId portNode = addObject(server, portInfo, ns, ifname, desc);

        addStringVariable(server, portNode, ns, "PortId", ifname);
        addUInt32Variable(server, portNode, ns, "PortIdSubtype", 5);
        addStringVariable(server, portNode, ns, "PortDescription",
                         "TSN communication port");
        addUInt32Variable(server, portNode, ns, "AdminStatus", 2);

        printf("[SERVER]   ✓ PortInfo/%s created\n", ifname);

        /* ─────────────────────────────────────────────────────
         * RemoteSystemsData: usa vicini dinamici o fallback
         * ───────────────────────────────────────────────────── */
        UA_NodeId remoteSys = addFolder(server, portNode, ns, "RemoteSystemsData");

        if (topology->neighborCount > 0) {
            /* Usa vicini LLDP scoperti dinamicamente */
            for (int j = 0; j < topology->neighborCount; j++) {
                LldpNeighbor *n = &topology->neighbors[j];

                char remoteName[64];
                snprintf(remoteName, sizeof(remoteName), "RemoteSystem%d", j + 1);

                char remoteDesc[256];
                snprintf(remoteDesc, sizeof(remoteDesc), "LLDP neighbor: %s",
                         n->systemName[0] ? n->systemName : "Unknown");

                UA_NodeId remoteNode = addObject(server, remoteSys, ns, remoteName, remoteDesc);

                addByteStringVariable(server, remoteNode, ns, "RemoteChassisId", n->chassisId, 6);
                addUInt32Variable(server, remoteNode, ns, "RemoteChassisIdSubtype", n->chassisIdSubtype);
                addStringVariable(server, remoteNode, ns, "RemotePortId",
                                n->portId[0] ? n->portId : "unknown");
                addUInt32Variable(server, remoteNode, ns, "RemotePortIdSubtype", n->portIdSubtype);
                addStringVariable(server, remoteNode, ns, "RemoteSystemName",
                                n->systemName[0] ? n->systemName : "Unknown");
                addStringVariable(server, remoteNode, ns, "RemoteSystemDescription",
                                n->systemDescription[0] ? n->systemDescription : "No description");
                addStringVariable(server, remoteNode, ns, "RemotePortDescription",
                                n->portDescription[0] ? n->portDescription : "No description");
                addStringVariable(server, remoteNode, ns, "RemoteManagementAddress",
                                n->managementAddress[0] ? n->managementAddress : "0.0.0.0");
                addUInt32Variable(server, remoteNode, ns, "RemoteManagementAddressSubtype",
                                n->managementAddressSubtype);
                addUInt32Variable(server, remoteNode, ns, "RemoteSystemCapabilitiesSupported",
                                n->capabilitiesSupported);
                addUInt32Variable(server, remoteNode, ns, "RemoteSystemCapabilitiesEnabled",
                                n->capabilitiesEnabled);
                addUInt32Variable(server, remoteNode, ns, "TimeToLive", n->timeToLive);
                addUInt32Variable(server, remoteNode, ns, "TimeMark", 123456 + j * 100);

                printf("[SERVER]   ✓ RemoteSystem%d: %s (MAC: %02X:%02X:%02X:%02X:%02X:%02X) [DYNAMIC]\n",
                       j + 1,
                       n->systemName[0] ? n->systemName : "Unknown",
                       n->chassisId[0], n->chassisId[1], n->chassisId[2],
                       n->chassisId[3], n->chassisId[4], n->chassisId[5]);
            }
        } else {
            /* Fallback: vicini statici per testing */
            printf("[SERVER]   Using static fallback neighbors\n");

            /* RemoteSystem1: TSN Switch */
            UA_NodeId remote1 = addObject(server, remoteSys, ns, "RemoteSystem1",
                                           "TSN Switch connected on port 3 (static)");

            UA_Byte remoteChassisId1[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
            addByteStringVariable(server, remote1, ns, "RemoteChassisId", remoteChassisId1, 6);
            addUInt32Variable(server, remote1, ns, "RemoteChassisIdSubtype", 4);
            addStringVariable(server, remote1, ns, "RemotePortId", "port3");
            addUInt32Variable(server, remote1, ns, "RemotePortIdSubtype", 5);
            addStringVariable(server, remote1, ns, "RemoteSystemName", "TSN-Switch-01");
            addStringVariable(server, remote1, ns, "RemoteSystemDescription",
                             "Industrial TSN Switch - 8-port Gigabit");
            addStringVariable(server, remote1, ns, "RemotePortDescription",
                             "Port 3: Sensor connection");
            addStringVariable(server, remote1, ns, "RemoteManagementAddress", "192.168.100.1");
            addUInt32Variable(server, remote1, ns, "RemoteManagementAddressSubtype", 1);
            addUInt32Variable(server, remote1, ns, "RemoteSystemCapabilitiesSupported", 0x104);
            addUInt32Variable(server, remote1, ns, "RemoteSystemCapabilitiesEnabled", 0x104);
            addUInt32Variable(server, remote1, ns, "TimeToLive", 120);
            addUInt32Variable(server, remote1, ns, "TimeMark", 123456);

            printf("[SERVER]   ✓ RemoteSystem1: TSN-Switch-01 (AA:BB:CC:11:22:33) [STATIC]\n");

            /* RemoteSystem2: UAFX Sensor */
            UA_NodeId remote2 = addObject(server, remoteSys, ns, "RemoteSystem2",
                                           "UAFX Pressure Sensor (static)");

            UA_Byte remoteChassisId2[6] = {0xDD, 0xEE, 0xFF, 0x44, 0x55, 0x66};
            addByteStringVariable(server, remote2, ns, "RemoteChassisId", remoteChassisId2, 6);
            addUInt32Variable(server, remote2, ns, "RemoteChassisIdSubtype", 4);
            addStringVariable(server, remote2, ns, "RemotePortId", "eth0");
            addUInt32Variable(server, remote2, ns, "RemotePortIdSubtype", 5);
            addStringVariable(server, remote2, ns, "RemoteSystemName", "PressureSensorUAFX");
            addStringVariable(server, remote2, ns, "RemoteSystemDescription",
                             "UAFX Pressure Sensor - AcmeCorp PressureSensor-2000");
            addStringVariable(server, remote2, ns, "RemotePortDescription",
                             "Primary Ethernet port for TSN communication");
            addStringVariable(server, remote2, ns, "RemoteManagementAddress", "192.168.100.5");
            addUInt32Variable(server, remote2, ns, "RemoteManagementAddressSubtype", 1);
            addUInt32Variable(server, remote2, ns, "RemoteSystemCapabilitiesSupported", 0x80);
            addUInt32Variable(server, remote2, ns, "RemoteSystemCapabilitiesEnabled", 0x80);
            addUInt32Variable(server, remote2, ns, "TimeToLive", 120);
            addUInt32Variable(server, remote2, ns, "TimeMark", 123789);

            printf("[SERVER]   ✓ RemoteSystem2: PressureSensorUAFX (DD:EE:FF:44:55:66) [STATIC]\n");
        }
    }

    printf("[SERVER] ✓ LLDP Information complete\n\n");
}

/* ═══════════════════════════════════════════════════════════
 * Build UAFX AddressSpace
 * ═══════════════════════════════════════════════════════════ */

static void buildUAFXAddressSpace(UA_Server *server, TopologyData *topology) {
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

    /* ─── 7. NetworkInterfaces/ ──────────────────────────── */
    /* UAFX Part 82, 6.5.3: rappresentazione interfacce di rete */
    buildNetworkInterfaces(server, acNode, NS_LOCAL, topology);

    /* ─── 8. LldpInformation/ ────────────────────────────── */
    /* UAFX Part 82, 6.5.2: informazioni LLDP locali e remote */
    buildLldpInformation(server, acNode, NS_LOCAL, topology);

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
    printf("║  OPC UA FX Temperature Server with LLDP           ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");

    /* Esegui topology discovery */
    TopologyData topology;
    performTopologyDiscovery(&topology);

    /* Crea server */
    UA_Server *server = UA_Server_new();
    UA_ServerConfig *config = UA_Server_getConfig(server);
    UA_String hostname = UA_String_fromChars("192.168.100.4");
    UA_ServerConfig_setMinimal(config, 4840, &hostname);

    /* Configurazione ApplicationDescription */
    config->applicationDescription.applicationType = UA_APPLICATIONTYPE_SERVER;

    UA_String_clear(&config->applicationDescription.applicationUri);
    config->applicationDescription.applicationUri =
        UA_String_fromChars("urn:example:uafx:temperature-sensor");

    UA_LocalizedText_clear(&config->applicationDescription.applicationName);
    config->applicationDescription.applicationName =
        UA_LOCALIZEDTEXT_ALLOC("en-US", "UAFX Temperature Sensor with LLDP");

    /* Configurazione mDNS */
    config->mdnsEnabled = true;
    config->mdnsConfig.mdnsServerName = UA_String_fromChars("TemperatureSensorUAFX");

    /* Capacità UAFX */
    config->mdnsConfig.serverCapabilitiesSize = 1;
    UA_String *caps = (UA_String *) UA_Array_new(1, &UA_TYPES[UA_TYPES_STRING]);
    caps[0] = UA_String_fromChars("UAFX");
    config->mdnsConfig.serverCapabilities = caps;
    config->mdnsInterfaceIP = hostname;

    /* Verifica mDNS */
#ifdef UA_ENABLE_DISCOVERY_MULTICAST
    printf("[SERVER] ✓ mDNS Discovery: ENABLED\n");
    printf("[SERVER]   Service: _opcua-tcp._tcp.local\n");
    printf("[SERVER]   Capability: UAFX\n\n");
#else
    printf("[SERVER] ⚠ mDNS Discovery: DISABLED\n");
    printf("[SERVER]   (Server still accessible via direct connection)\n\n");
#endif

    /* Costruisci AddressSpace UAFX con dati di topology discovery */
    buildUAFXAddressSpace(server, &topology);

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

    if (topology.discoverySuccess) {
        printf("Topology Discovery Summary:\n");
        printf("  Network Interfaces: %d discovered\n", topology.interfaceCount);
        printf("  LLDP Neighbors:     %d discovered\n", topology.neighborCount);
    } else {
        printf("Topology Discovery Summary:\n");
        printf("  ⚠ Using static fallback data for testing\n");
    }
    printf("\n");

    printf("UAFX Structure with Dynamic LLDP Topology:\n");
    printf("  Objects/\n");
    printf("  └── FxRoot/\n");
    printf("      └── TemperatureSensor/ [AutomationComponent]\n");
    printf("          ├── ConformanceName: urn:example:uafx:temperature-sensor:v1.0\n");
    printf("          ├── AggregatedHealth: 0\n");
    printf("          ├── Assets/\n");
    printf("          │   └── SensorHardware/\n");
    printf("          ├── FunctionalEntities/\n");
    printf("          │   └── TemperatureReadingFE/\n");
    printf("          │       └── OutputData/\n");
    printf("          │           └── Temperature: ~20°C (dynamic)\n");
    printf("          ├── ComponentCapabilities/\n");
    printf("          │   └── MaxConnections: 4\n");
    printf("          ├── NetworkInterfaces/ [Part 82, 6.5.3]\n");
    for (int i = 0; i < topology.interfaceCount; i++) {
        printf("          │   └── %s/ %s\n",
               topology.interfaces[i].name,
               topology.interfaceCount > 1 && i < topology.interfaceCount - 1 ? "" : "");
    }
    if (topology.interfaceCount == 0) {
        printf("          │   └── eth0/ [STATIC FALLBACK]\n");
    }
    printf("          └── LldpInformation/ [Part 82, 6.5.2]\n");
    printf("              ├── LocalSystemData/\n");
    printf("              └── PortInfo/\n");
    if (topology.neighborCount > 0) {
        printf("                  └── RemoteSystemsData/ [%d neighbor(s) - DYNAMIC]\n",
               topology.neighborCount);
    } else {
        printf("                  └── RemoteSystemsData/ [2 neighbors - STATIC FALLBACK]\n");
    }
    printf("\n");

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
