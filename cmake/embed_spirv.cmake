if(NOT DEFINED GENERATED_DIR)
    message(FATAL_ERROR "embed_spirv.cmake requires GENERATED_DIR")
endif()

function(embed_shader SOURCE NAME OUTPUT_VAR)
    set(SPIRV "${GENERATED_DIR}/${NAME}.spv")
    set(HEADER "${GENERATED_DIR}/${NAME}.spv.hpp")

    add_custom_command(
        OUTPUT "${HEADER}"
        BYPRODUCTS "${SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
        COMMAND Vulkan::glslc -O --target-env=vulkan1.3 "${SOURCE}" -o "${SPIRV}"
        COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${SPIRV}"
            "-DOUTPUT=${HEADER}"
            "-DNAME=${NAME}_spirv"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
        DEPENDS "${SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
        VERBATIM
    )

    set(${OUTPUT_VAR} "${HEADER}" PARENT_SCOPE)
endfunction()

function(embed_binary_file SOURCE NAME OUTPUT_VAR)
    set(HEADER "${GENERATED_DIR}/${NAME}.hpp")
    add_custom_command(
        OUTPUT "${HEADER}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
        COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${SOURCE}"
            "-DOUTPUT=${HEADER}"
            "-DNAME=${NAME}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
        DEPENDS "${SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
        VERBATIM
    )
    set(${OUTPUT_VAR} "${HEADER}" PARENT_SCOPE)
endfunction()
