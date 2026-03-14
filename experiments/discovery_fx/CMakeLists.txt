cmake_minimum_required(VERSION 3.10)
project(UAFX_Server C)

set(CMAKE_C_STANDARD 99)

# 1. PERCORSI
set(OPEN_TOOLS "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/open62541/tools")
set(NODESETS "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/UA-Nodeset")

# ---------------------------------------------------------
# 2. GENERAZIONE DEI TIPI DI DATO (I 3 Pilastri)
# ---------------------------------------------------------

# A. Genera i Tipi per Device Integration (DI)
add_custom_command(
    OUTPUT types_di_generated.c types_di_generated.h
    COMMAND python3 ${OPEN_TOOLS}/generate_datatypes.py
            --namespace=2:http://opcfoundation.org/UA/DI/
            --type-bsd ${NODESETS}/DI/Opc.Ua.Di.Types.bsd
            --type-csv ${NODESETS}/DI/Opc.Ua.Di.NodeIds.csv
            --no-builtin
            types_di
    DEPENDS ${NODESETS}/DI/Opc.Ua.Di.Types.bsd
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT ">>> Generazione tipi DI..."
)

# B1. Genera i Tipi per UAFX DATA
add_custom_command(
    OUTPUT types_uafx_data_generated.c types_uafx_data_generated.h
    COMMAND python3 ${OPEN_TOOLS}/generate_datatypes.py
            --namespace=3:http://opcfoundation.org/UA/FX/Data/
            --namespaceMap=2:http://opcfoundation.org/UA/DI/
            --type-bsd ${NODESETS}/UAFX/opc.ua.fx.data.nodeset2.bsd
            --type-csv ${NODESETS}/UAFX/opc.ua.fx.data.nodeset2.csv
            --no-builtin
            --import "UA_TYPES#${NODESETS}/Schema/Opc.Ua.Types.bsd"
            --import "types_di#${NODESETS}/DI/Opc.Ua.Di.Types.bsd"
            types_uafx_data
    DEPENDS ${NODESETS}/UAFX/opc.ua.fx.data.nodeset2.bsd types_di_generated.h
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT ">>> Generazione tipi UAFX DATA..."
)

# B2. Genera i Tipi per UAFX AC
add_custom_command(
    OUTPUT types_uafx_ac_generated.c types_uafx_ac_generated.h
    COMMAND python3 ${OPEN_TOOLS}/generate_datatypes.py
            --namespace=4:http://opcfoundation.org/UA/FX/AC/
            --namespaceMap=2:http://opcfoundation.org/UA/DI/
            --namespaceMap=3:http://opcfoundation.org/UA/FX/Data/
            --type-bsd ${NODESETS}/UAFX/opc.ua.fx.ac.nodeset2.bsd
            --type-csv ${NODESETS}/UAFX/opc.ua.fx.ac.nodeset2.csv
            --no-builtin
            --import "UA_TYPES#${NODESETS}/Schema/Opc.Ua.Types.bsd"
            --import "types_di#${NODESETS}/DI/Opc.Ua.Di.Types.bsd"
            --import "types_uafx_data#${NODESETS}/UAFX/opc.ua.fx.data.nodeset2.bsd"
            types_uafx_ac
    DEPENDS ${NODESETS}/UAFX/opc.ua.fx.ac.nodeset2.bsd types_uafx_data_generated.h types_di_generated.h
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT ">>> Generazione tipi UAFX AC..."
)

# ---------------------------------------------------------
# 3. COMPILAZIONE DELL'ADDRESS SPACE
# ---------------------------------------------------------
add_custom_command(
    OUTPUT my_uafx_model.c my_uafx_model.h
    COMMAND python3 ${OPEN_TOOLS}/nodeset_compiler/nodeset_compiler.py
            --types-array=UA_TYPES             # per Schema XML
            --types-array=UA_TYPES_DI          # per DI XML
            --types-array=UA_TYPES_UAFX_DATA   # per FX DATA XML
            --types-array=UA_TYPES_UAFX_AC     # per FX AC XML
            --existing ${NODESETS}/Schema/Opc.Ua.NodeSet2.xml
            --xml ${NODESETS}/DI/Opc.Ua.Di.NodeSet2.xml
            --xml ${NODESETS}/UAFX/opc.ua.fx.data.nodeset2.xml
            --xml ${NODESETS}/UAFX/opc.ua.fx.ac.nodeset2.xml
            my_uafx_model
    DEPENDS types_uafx_ac_generated.h types_uafx_data_generated.h types_di_generated.h
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT ">>> Compilazione Address Space UAFX..."
)

# ---------------------------------------------------------
# 4. CREAZIONE DELL'ESEGUIBILE
# ---------------------------------------------------------
add_executable(uafx_server
    ${CMAKE_CURRENT_SOURCE_DIR}/server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/open62541.c
    ${CMAKE_CURRENT_BINARY_DIR}/types_di_generated.c
    ${CMAKE_CURRENT_BINARY_DIR}/types_uafx_data_generated.c
    ${CMAKE_CURRENT_BINARY_DIR}/types_uafx_ac_generated.c
    ${CMAKE_CURRENT_BINARY_DIR}/my_uafx_model.c
)

target_compile_definitions(uafx_server PRIVATE UA_ENABLE_AMALGAMATION)
target_include_directories(uafx_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(uafx_server pthread)
