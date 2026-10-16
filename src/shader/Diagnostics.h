#pragma once

#include "rhi/RHI.h"

#include <string>
#include <vector>

namespace shader {

// Parses driver compile logs into per-line diagnostics. Handles the common formats:
//   Apple / glslang : "ERROR: 0:12: 'x' : undeclared identifier"
//   Mesa            : "0:12(5): error: ..."
//   NVIDIA          : "0(12) : error C1008: ..."
std::vector<rhi::Diagnostic> parseCompileLog(const std::string& log);

}  // namespace shader
