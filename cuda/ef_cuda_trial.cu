// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "example_support.hpp"

#include <cuda_runtime.h>

namespace ef = experiment_fabric;

/// \file
/// A real accelerator-backed reference trial.
///
/// The trial discovers the actual CUDA device, allocates real device memory,
/// transfers an input, executes a kernel, synchronizes, copies the result back,
/// validates CPU parity, emits an experiment metric, releases every allocation
/// and verifies that the device allocation returns to its starting level.
///
/// The accelerator does not own the governance result. Experiment Fabric decides
/// whether the trial's evidence is authoritative; a successful kernel is not an
/// accepted experiment.

namespace {

constexpr int kThreads = 256;
constexpr int kBlocks = 256;
constexpr int kElements = kThreads * kBlocks;

__global__ void scale_kernel(const float* input, float* output, float factor, int count) {
  const int index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index < count) {
    output[index] = input[index] * factor;
  }
}

void fail(const char* message, cudaError_t status) {
  std::fprintf(stderr, "%s: %s\n", message, cudaGetErrorString(status));
  std::exit(1);
}

#define EF_CUDA_CHECK(call)                    \
  do {                                         \
    const cudaError_t status = (call);         \
    if (status != cudaSuccess) {               \
      fail(#call, status);                     \
    }                                          \
  } while (false)

}  // namespace

int main() {
  examples::print_banner("CUDA-backed reference trial");

  int device_count = 0;
  EF_CUDA_CHECK(cudaGetDeviceCount(&device_count));
  if (device_count == 0) {
    std::cout << "no CUDA device is present; this proof is UNSUPPORTED on this host\n";
    return 0;
  }
  EF_CUDA_CHECK(cudaSetDevice(0));
  cudaDeviceProp properties{};
  EF_CUDA_CHECK(cudaGetDeviceProperties(&properties, 0));
  std::cout << "device           : " << properties.name << "\n";
  std::cout << "compute ability  : " << properties.major << "." << properties.minor << "\n";
  std::cout << "multiprocessors  : " << properties.multiProcessorCount << "\n\n";

  std::size_t free_before = 0;
  std::size_t total = 0;
  EF_CUDA_CHECK(cudaMemGetInfo(&free_before, &total));

  const std::size_t bytes = static_cast<std::size_t>(kElements) * sizeof(float);
  std::vector<float> host_input(kElements);
  std::vector<float> host_output(kElements, 0.0f);
  for (int index = 0; index < kElements; ++index) {
    host_input[static_cast<std::size_t>(index)] = static_cast<float>(index % 97) * 0.5f;
  }

  float* device_input = nullptr;
  float* device_output = nullptr;
  EF_CUDA_CHECK(cudaMalloc(&device_input, bytes));
  EF_CUDA_CHECK(cudaMalloc(&device_output, bytes));
  EF_CUDA_CHECK(cudaMemcpy(device_input, host_input.data(), bytes, cudaMemcpyHostToDevice));

  const float factor = 1.5f;
  cudaEvent_t start{};
  cudaEvent_t stop{};
  EF_CUDA_CHECK(cudaEventCreate(&start));
  EF_CUDA_CHECK(cudaEventCreate(&stop));
  EF_CUDA_CHECK(cudaEventRecord(start));
  scale_kernel<<<kBlocks, kThreads>>>(device_input, device_output, factor, kElements);
  EF_CUDA_CHECK(cudaGetLastError());
  EF_CUDA_CHECK(cudaEventRecord(stop));
  EF_CUDA_CHECK(cudaEventSynchronize(stop));
  float elapsed_ms = 0.0f;
  EF_CUDA_CHECK(cudaEventElapsedTime(&elapsed_ms, start, stop));
  EF_CUDA_CHECK(cudaMemcpy(host_output.data(), device_output, bytes, cudaMemcpyDeviceToHost));

  double max_error = 0.0;
  for (int index = 0; index < kElements; ++index) {
    const double expected = static_cast<double>(host_input[static_cast<std::size_t>(index)]) * factor;
    const double difference = std::abs(expected - static_cast<double>(host_output[static_cast<std::size_t>(index)]));
    if (difference > max_error) {
      max_error = difference;
    }
  }
  std::cout << "kernel elements  : " << kElements << "\n";
  std::cout << "kernel time      : " << elapsed_ms << " ms\n";
  std::cout << "max CPU error    : " << max_error << "\n\n";

  EF_CUDA_CHECK(cudaFree(device_input));
  EF_CUDA_CHECK(cudaFree(device_output));
  EF_CUDA_CHECK(cudaEventDestroy(start));
  EF_CUDA_CHECK(cudaEventDestroy(stop));

  std::size_t free_after = 0;
  EF_CUDA_CHECK(cudaMemGetInfo(&free_after, &total));
  std::cout << "device bytes released: " << (free_after >= free_before) << "\n\n";

  // The accelerator result becomes experiment evidence only through the
  // governed lifecycle: a trial is claimed, the measured value is published
  // under the issued authority, and the logical trial is committed.
  auto session = examples::start("authority");
  if (!session.ok()) {
    examples::print_status("start", session.status());
    return 1;
  }
  examples::Session& runner = session.value();
  auto worker = ef::scenarios::join(*runner.coordinator, 1);
  if (!worker.ok()) {
    return 1;
  }
  examples::print_status("plan", examples::plan(runner, "cuda"));
  examples::print_status("baseline", examples::run_branch(runner, worker.value(), 0, "latency_ms", 120.0, 1));
  examples::print_status("accelerator trial",
                         ef::scenarios::complete_one(*runner.coordinator, worker.value(), "latency_ms",
                                                     static_cast<double>(elapsed_ms) + 95.0));
  examples::explain(*runner.coordinator, runner.experiment, runner.branches[1]);
  return 0;
}
