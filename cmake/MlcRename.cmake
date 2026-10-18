# Script mode: prefix every global symbol defined in INPUT, writing OUTPUT. See MlcSecondCopy.cmake.
execute_process(COMMAND ${NM} -g --defined-only --format=just-symbols ${INPUT}
        OUTPUT_VARIABLE symbols COMMAND_ERROR_IS_FATAL ANY)
string(REGEX REPLACE "\n$" "" symbols "${symbols}")
string(REGEX REPLACE "([^\n]+)" "\\1 ${PREFIX}\\1" map "${symbols}")
file(WRITE ${OUTPUT}.syms "${map}\n")
execute_process(COMMAND ${OBJCOPY} --redefine-syms=${OUTPUT}.syms ${INPUT} ${OUTPUT} COMMAND_ERROR_IS_FATAL ANY)
