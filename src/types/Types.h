#pragma once

/// Umbrella header for all Ultra core types.
///
/// Include this single header to access:
///   - Result<T, E>       — Generic success/error type
///   - StructuredError     — Classified error with severity
///   - StructuredOutput    — Command result wrapper
///   - Confidence          — 3-axis confidence scoring
///   - Delta<T>            — Generic state change
///   - BranchId            — UUID-based branch identifier
///   - Timestamp           — ISO-8601 monotonic timestamp
///   - Serialization       — JSON to_json/from_json for all types

#include "Result.h"
#include "StructuredError.h"
#include "StructuredOutput.h"
#include "Confidence.h"
#include "Delta.h"
#include "BranchId.h"
#include "Timestamp.h"
#include "Serialization.h"
