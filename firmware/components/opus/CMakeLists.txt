set(OPUS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../third_party/opus-1.5.2)

set(OPUS_SRC
    ${OPUS_DIR}/src/opus.c
    ${OPUS_DIR}/src/opus_decoder.c
    ${OPUS_DIR}/src/opus_encoder.c
    ${OPUS_DIR}/src/repacketizer.c
    ${OPUS_DIR}/src/analysis.c
    ${OPUS_DIR}/src/mlp.c
    ${OPUS_DIR}/src/mlp_data.c
    ${OPUS_DIR}/src/extensions.c
)
file(GLOB CELT_SRC ${OPUS_DIR}/celt/*.c)
list(FILTER CELT_SRC EXCLUDE REGEX ".*opus_custom_demo\.c$")
file(GLOB SILK_SRC ${OPUS_DIR}/silk/*.c ${OPUS_DIR}/silk/fixed/*.c)

idf_component_register(
    SRCS ${OPUS_SRC} ${CELT_SRC} ${SILK_SRC}
    INCLUDE_DIRS ${OPUS_DIR}/include
    PRIV_INCLUDE_DIRS ${OPUS_DIR}/celt ${OPUS_DIR}/silk ${OPUS_DIR}/silk/fixed ${OPUS_DIR}/src
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE OPUS_BUILD FIXED_POINT=1 VAR_ARRAYS)
