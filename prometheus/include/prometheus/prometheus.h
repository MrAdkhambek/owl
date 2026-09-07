#pragma once

// Prometheus text-exposition metrics: counters, gauges, histograms, and the
// HTTP RED recorder. Standalone -- it depends on no web framework; callers
// wire record_request and dump() into their own HTTP layer.

#include "prometheus/http.h"
#include "prometheus/registry.h"
