// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef EXPERIMENT_FABRIC_EXPORT_HPP
#define EXPERIMENT_FABRIC_EXPORT_HPP

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(EXPERIMENT_FABRIC_SHARED)
#if defined(EXPERIMENT_FABRIC_BUILDING)
#define EF_API __declspec(dllexport)
#else
#define EF_API __declspec(dllimport)
#endif
#else
#define EF_API
#endif
#else
#if defined(EXPERIMENT_FABRIC_BUILDING) && defined(EXPERIMENT_FABRIC_SHARED)
#define EF_API __attribute__((visibility("default")))
#else
#define EF_API
#endif
#endif

#endif  // EXPERIMENT_FABRIC_EXPORT_HPP
