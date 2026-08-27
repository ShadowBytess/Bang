if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED NAME)
    message(FATAL_ERROR "embed_binary requires INPUT, OUTPUT and NAME")
endif()

file(READ "${INPUT}" content HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
string(REGEX REPLACE ",$" "" bytes "${bytes}")

file(WRITE "${OUTPUT}"
    "#include <cstddef>\n#include <cstdint>\n\n"
    "namespace {\n\n"
    "constexpr unsigned char ${NAME}_data[] = {${bytes}};\n\n"
    "} // namespace\n\n"
    "const std::uint8_t* ${NAME}_bytes()\n{\n"
    "    return reinterpret_cast<const std::uint8_t*>(${NAME}_data);\n"
    "}\n\n"
    "std::size_t ${NAME}_size()\n{\n"
    "    return sizeof(${NAME}_data);\n"
    "}\n"
)
