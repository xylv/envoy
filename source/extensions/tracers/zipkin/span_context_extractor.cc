#include "extensions/tracers/zipkin/span_context_extractor.h"

#include "common/common/assert.h"
#include "common/common/utility.h"

#include "extensions/tracers/zipkin/span_context.h"
#include "extensions/tracers/zipkin/zipkin_core_constants.h"

namespace Envoy {
namespace Extensions {
namespace Tracers {
namespace Zipkin {
namespace {
constexpr int FormatMaxLength = 32 + 1 + 16 + 3 + 16; // traceid128-spanid-1-parentid
bool validSamplingFlags(char c) {
  if (c == '1' || c == '0' || c == 'd') {
    return true;
  }
  return false;
}

bool getSamplingFlags(char c, const Tracing::Decision tracing_decision) {
  if (validSamplingFlags(c)) {
    return c == '0' ? false : true;
  } else {
    return tracing_decision.traced;
  }
}

} // namespace

SpanContextExtractor::SpanContextExtractor(Http::HeaderMap& request_headers)
    : request_headers_(request_headers) {}

SpanContextExtractor::~SpanContextExtractor() {}

bool SpanContextExtractor::extractSampled(const Tracing::Decision tracing_decision) {
  bool sampled(false);
  auto b3_header_entry = request_headers_.get(ZipkinCoreConstants::get().B3);
  if (b3_header_entry) {
    absl::string_view b3 = b3_header_entry->value().getStringView();
    int sampled_pos = 0;
    switch (b3.length()) {
    case 1:
      break;
    case 35: // 16 + 1 + 16 + 2
      sampled_pos = 34;
      break;
    case 51: // 32 + 1 + 16 + 2
      sampled_pos = 50;
      break;
    case 52: // 16 + 1 + 16 + 2 + 1 + 16
      sampled_pos = 34;
      break;
    case 68: // 32 + 1 + 16 + 2 + 1 + 16
      sampled_pos = 50;
      break;
    default:
      return tracing_decision.traced;
    }
    return getSamplingFlags(b3[sampled_pos], tracing_decision);
  }

  auto x_b3_sampled_entry = request_headers_.get(ZipkinCoreConstants::get().X_B3_SAMPLED);
  if (!x_b3_sampled_entry) {
    return tracing_decision.traced;
  }
  // Checking if sampled flag has been specified. Also checking for 'true' value, as some old
  // zipkin tracers may still use that value, although should be 0 or 1.
  absl::string_view xb3_sampled = x_b3_sampled_entry->value().getStringView();
  sampled = xb3_sampled == ZipkinCoreConstants::get().SAMPLED || xb3_sampled == "true";
  return sampled;
}

std::pair<SpanContext, bool> SpanContextExtractor::extractSpanContext(bool is_sampled) {
  if (request_headers_.get(ZipkinCoreConstants::get().B3)) {
    return extractSpanContextFromB3SingleFormat(is_sampled);
  }
  uint64_t trace_id(0);
  uint64_t trace_id_high(0);
  uint64_t span_id(0);
  uint64_t parent_id(0);

  auto b3_trace_id_entry = request_headers_.get(ZipkinCoreConstants::get().X_B3_TRACE_ID);
  auto b3_span_id_entry = request_headers_.get(ZipkinCoreConstants::get().X_B3_SPAN_ID);
  if (b3_span_id_entry && b3_trace_id_entry) {
    // Extract trace id - which can either be 128 or 64 bit. For 128 bit,
    // it needs to be divided into two 64 bit numbers (high and low).
    const std::string tid = b3_trace_id_entry->value().c_str();
    if (b3_trace_id_entry->value().size() == 32) {
      const std::string high_tid = tid.substr(0, 16);
      const std::string low_tid = tid.substr(16, 16);
      if (!StringUtil::atoull(high_tid.c_str(), trace_id_high, 16) ||
          !StringUtil::atoull(low_tid.c_str(), trace_id, 16)) {
        throw ExtractorException(
            fmt::format("Invalid traceid_high {} or tracid {}", high_tid.c_str(), low_tid.c_str()));
      }
    } else if (!StringUtil::atoull(tid.c_str(), trace_id, 16)) {
      throw ExtractorException(fmt::format("Invalid trace_id {}", tid.c_str()));
    }

    const std::string spid = b3_span_id_entry->value().c_str();
    if (!StringUtil::atoull(spid.c_str(), span_id, 16)) {
      throw ExtractorException(fmt::format("Invalid span id {}", spid.c_str()));
    }

    auto b3_parent_id_entry = request_headers_.get(ZipkinCoreConstants::get().X_B3_PARENT_SPAN_ID);
    if (b3_parent_id_entry) {
      const std::string pspid = b3_parent_id_entry->value().c_str();
      if (!StringUtil::atoull(pspid.c_str(), parent_id, 16)) {
        throw ExtractorException(fmt::format("Invalid parent span id {}", pspid.c_str()));
      }
    }
  } else {
    return std::pair<SpanContext, bool>(SpanContext(), false);
  }

  return std::pair<SpanContext, bool>(
      SpanContext(trace_id_high, trace_id, span_id, parent_id, is_sampled), true);
}

std::pair<SpanContext, bool>
SpanContextExtractor::extractSpanContextFromB3SingleFormat(bool is_sampled) {
  auto b3_head_entry = request_headers_.get(ZipkinCoreConstants::get().B3);
  ASSERT(b3_head_entry);
  const std::string b3 = b3_head_entry->value().c_str();
  if (!b3.length()) {
    throw ExtractorException("Invalid input: empty");
  }

  if (b3.length() == 1) { // possibly sampling flags
    if (validSamplingFlags(b3[0])) {
      return std::pair<SpanContext, bool>(SpanContext(), false);
    }
    throw ExtractorException(fmt::format("Invalid input: invalid sampling flag {}", b3[0]));
  }

  if (b3.length() < 16 + 1 + 16 /* traceid64-spanid */) {
    throw ExtractorException("Invalid input: truncated");
  } else if (b3.length() > FormatMaxLength) {
    throw ExtractorException("Invalid input: too long");
  }

  uint64_t trace_id(0);
  uint64_t trace_id_high(0);
  uint64_t span_id(0);
  uint64_t parent_id(0);

  uint64_t pos = 0;

  const std::string trace_id_str = b3.substr(pos, 16);
  if (b3[pos + 32] == '-') {
    if (!StringUtil::atoull(trace_id_str.c_str(), trace_id_high, 16)) {
      throw ExtractorException(
          fmt::format("Invalid input: invalid trace id high {}", trace_id_str.c_str()));
    }
    pos += 16;
    const std::string trace_id_low_str = b3.substr(pos, 16);
    if (!StringUtil::atoull(trace_id_low_str.c_str(), trace_id, 16)) {
      throw ExtractorException(
          fmt::format("Invalid input: invalid trace id {}", trace_id_low_str.c_str()));
    }
  } else {
    if (!StringUtil::atoull(trace_id_str.c_str(), trace_id, 16)) {
      throw ExtractorException(
          fmt::format("Invalid input: invalid trace id {}", trace_id_str.c_str()));
    }
  }

  pos += 16; // traceId ended
  if (!(b3[pos++] == '-')) {
    throw ExtractorException("Invalid input: not exists span id");
  }

  const std::string span_id_str = b3.substr(pos, 16);
  if (!StringUtil::atoull(span_id_str.c_str(), span_id, 16)) {
    throw ExtractorException(fmt::format("Invalid input: invalid span id {}", span_id_str.c_str()));
  }
  pos += 16; // spanId ended

  if (b3.length() > pos) {
    // If we are at this point, we have more than just traceId-spanId.
    // If the sampling field is present, we'll have a delimiter 2 characters from now. Ex "-1"
    // If it is absent, but a parent ID is (which is strange), we'll have at least 17 characters.
    // Therefore, if we have less than two characters, the input is truncated.
    if (b3.length() == (pos + 1)) {
      throw ExtractorException("Invalid input: truncated");
    }

    if (!(b3[pos++] == '-')) {
      throw ExtractorException("Invalid input: not exists sampling field");
    }

    // If our position is at the end of the string, or another delimiter is one character past our
    // position, try to read sampled status.
    if (b3.length() == pos + 1 || ((b3.length() >= pos + 2) && (b3[pos + 1] == '-'))) {
      if (!validSamplingFlags(b3[pos])) {
        throw ExtractorException(fmt::format("Invalid input: invalid sampling flag {}", b3[pos]));
      }
      pos++; // consume the sampled status
    } else {
      throw ExtractorException("Invalid input: truncated");
    }

    if (b3.length() > pos) {
      // If we are at this point, we should have a parent ID, encoded as "-[0-9a-f]{16}"
      if (b3.length() != pos + 17) {
        throw ExtractorException("Invalid input: truncated");
      }

      ASSERT(b3[pos] == '-');
      pos++;

      const std::string parent_id_str = b3.substr(pos, b3.length() - pos);
      if (!StringUtil::atoull(parent_id_str.c_str(), parent_id, 16)) {
        throw ExtractorException(
            fmt::format("Invalid input: invalid parent id {}", parent_id_str.c_str()));
      }
    }
  }

  return std::pair<SpanContext, bool>(
      SpanContext(trace_id_high, trace_id, span_id, parent_id, is_sampled), true);
}

} // namespace Zipkin
} // namespace Tracers
} // namespace Extensions
} // namespace Envoy