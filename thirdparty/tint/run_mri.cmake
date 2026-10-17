# cmake -DAR=<llvm-ar|emar> -DMRI=<script> -P run_mri.cmake
# Runs an ar MRI script (stdin redirection without a shell).
execute_process(COMMAND "${AR}" -M INPUT_FILE "${MRI}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "${AR} -M ${MRI} failed: ${rc}")
endif()
