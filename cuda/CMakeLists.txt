# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

# The CUDA reference trial is built only when a CUDA toolkit is available. It
# demonstrates that an accelerator-backed trial can participate in the
# experiment lifecycle without changing the governance boundary: the runtime
# still decides whether the trial's evidence is authoritative.

add_executable(ef_cuda_trial ef_cuda_trial.cu)

set_target_properties(ef_cuda_trial PROPERTIES
  CUDA_STANDARD 20
  CUDA_STANDARD_REQUIRED ON
  CUDA_EXTENSIONS OFF
)

target_link_libraries(ef_cuda_trial PRIVATE experiment_fabric ef_scenarios)
target_include_directories(ef_cuda_trial PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../examples")

# The strict warning set is handed to the host compiler through -Xcompiler.
# Passing /W4 directly would make nvcc treat it as an input file name.
if(MSVC)
  target_compile_definitions(ef_cuda_trial PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(ef_cuda_trial PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:SHELL:-Xcompiler=/W4>
    $<$<COMPILE_LANGUAGE:CUDA>:SHELL:-Xcompiler=/WX>
    $<$<COMPILE_LANGUAGE:CUDA>:SHELL:-Xcompiler=/utf-8>
    $<$<COMPILE_LANGUAGE:CUDA>:SHELL:-Xcompiler=/Zc:__cplusplus>
  )
endif()
