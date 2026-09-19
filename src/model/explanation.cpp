// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "ncf/model/explanation.hpp"

#include <string>

namespace ncf {

namespace {

[[nodiscard]] std::string number(std::uint64_t value) { return std::to_string(value); }

[[nodiscard]] std::string threshold_op_text(ThresholdOp op) {
  return op == ThresholdOp::AtLeast ? "at-least" : "at-most";
}

}  // namespace

std::string Explanation::render(std::size_t max_bytes) const {
  std::string out;
  out.reserve(4096);
  std::size_t truncated = 0;

  const auto put = [&out, max_bytes](const std::string& line) -> bool {
    if (out.size() + line.size() + 1 > max_bytes) {
      return false;
    }
    out.append(line);
    out.push_back('\n');
    return true;
  };

  put("ncf-explanation format=" + number(kFormatVersion));
  put("domain=" + to_string(domain));
  put("state=" + std::string(to_string(state)) + " previous=" + std::string(to_string(previous_state)) +
      " transitioned=" + (transitioned ? "yes" : "no") + " authoritative=" + (authoritative ? "yes" : "no") +
      " requires_revalidation=" + (requires_revalidation ? "yes" : "no"));
  put("severity=" + std::string(to_string(severity)));
  put("evaluation=" + to_string(evaluation) + " epoch=" + to_string(epoch) + " tick=" + number(evaluated_tick));
  put("authority_generation=" + to_string(authority_generation) + " authority=" + authority.describe());
  put("policy=" + to_string(policy) + " fingerprint=" + policy_fingerprint.to_hex());
  put("topology_generation=" + to_string(topology_generation) +
      " capacity_generation=" + to_string(capacity_generation));
  put("reason=" + (reason_code.empty() ? std::string("unspecified") : reason_code));
  for (const std::string& detail : reason_detail) {
    if (!put("reason-detail=" + detail)) {
      ++truncated;
      break;
    }
  }
  put("evidence seen=" + number(samples_seen) + " accepted=" + number(samples_accepted) +
      " rejected=" + number(samples_rejected) + " duplicate=" + number(samples_duplicate));

  if (!put("scope begin")) {
    ++truncated;
  }
  for (const ResourceAssessment& assessment : resources) {
    std::string line = "resource=" + to_string(assessment.resource);
    line.append(" raw=");
    line.append(to_string(assessment.raw_severity));
    line.append(" coverage=");
    line.append(to_string(assessment.coverage));
    line.append(" stale=");
    line.append(assessment.stale ? "yes" : "no");
    line.append(" contradictory=");
    line.append(assessment.contradictory ? "yes" : "no");
    line.append(" samples=");
    line.append(number(assessment.samples_considered));
    line.append(" stale_samples=");
    line.append(number(assessment.stale_samples));
    line.append(" future_samples=");
    line.append(number(assessment.future_samples));
    line.append(" newest_tick=");
    line.append(number(assessment.newest_observed_tick));
    if (!put(line)) {
      ++truncated;
      break;
    }
  }

  for (const ThresholdBinding& binding : bindings) {
    std::string line = "binding resource=" + to_string(binding.resource);
    if (binding.queue.valid()) {
      line.append(" queue=");
      line.append(to_string(binding.queue));
    }
    line.append(" metric=");
    line.append(to_string(binding.metric));
    line.append(" value=");
    line.append(number(binding.value));
    line.append(" op=");
    line.append(threshold_op_text(binding.op));
    line.append(" watch=");
    line.append(number(binding.watch));
    line.append(" congested=");
    line.append(number(binding.congested));
    line.append(" severe=");
    line.append(number(binding.severe));
    line.append(" level=");
    line.append(to_string(binding.level));
    line.append(" required=");
    line.append(binding.required ? "yes" : "no");
    line.append(" stale=");
    line.append(binding.stale ? "yes" : "no");
    line.append(" future=");
    line.append(binding.future ? "yes" : "no");
    line.append(" publisher=");
    line.append(to_string(binding.publisher));
    line.append(" observed_tick=");
    line.append(number(binding.observed_tick));
    if (!put(line)) {
      ++truncated;
      break;
    }
  }

  for (const PropagationEdge& edge : propagation) {
    std::string line = "propagation from=" + to_string(edge.from) + " to=" + to_string(edge.to);
    line.append(" hops=");
    line.append(number(edge.hops));
    line.append(" pressure_ppm=");
    line.append(number(edge.pressure_ppm));
    line.append(" induced=");
    line.append(to_string(edge.induced));
    line.append(" applied=");
    line.append(edge.applied ? "yes" : "no");
    if (edge.via_path.valid()) {
      line.append(" via=");
      line.append(to_string(edge.via_path));
    }
    if (!put(line)) {
      ++truncated;
      break;
    }
  }

  for (const ConflictReport& conflict : conflicts) {
    std::string line = "conflict resource=" + to_string(conflict.resource) + " metric=" + std::string(to_string(conflict.metric));
    line.append(" low=");
    line.append(number(conflict.low_value));
    line.append(" high=");
    line.append(number(conflict.high_value));
    line.append(" spread=");
    line.append(number(conflict.spread));
    line.append(" low_publisher=");
    line.append(to_string(conflict.low_publisher));
    line.append(" high_publisher=");
    line.append(to_string(conflict.high_publisher));
    line.append(" resolved=");
    line.append(conflict.resolved ? "yes" : "no");
    if (!put(line)) {
      ++truncated;
      break;
    }
  }

  for (const StaleReport& stale_report : stale) {
    std::string line = "stale resource=" + to_string(stale_report.resource) +
                       " metric=" + std::string(to_string(stale_report.metric));
    line.append(" publisher=");
    line.append(to_string(stale_report.publisher));
    line.append(" observed_tick=");
    line.append(number(stale_report.observed_tick));
    line.append(" age=");
    line.append(number(stale_report.age_ticks));
    line.append(" allowed_age=");
    line.append(number(stale_report.allowed_age_ticks));
    line.append(" from_future=");
    line.append(stale_report.from_future ? "yes" : "no");
    line.append(" beyond_skew=");
    line.append(stale_report.beyond_future_skew ? "yes" : "no");
    if (!put(line)) {
      ++truncated;
      break;
    }
  }

  for (const InterventionIntent& intent : authorized) {
    std::string line = "authorized kind=" + std::string(to_string(intent.kind)) + " domain=" + to_string(intent.domain);
    if (intent.resource.valid()) {
      line.append(" resource=");
      line.append(to_string(intent.resource));
    }
    line.append(" basis=");
    line.append(to_string(intent.basis_state));
    line.append(" parameter=");
    line.append(number(intent.parameter));
    line.append(" clamped=");
    line.append(intent.parameter_clamped ? "yes" : "no");
    line.append(" evidence=");
    line.append(to_string(intent.evidence));
    line.append(" expires_tick=");
    line.append(number(intent.expires_tick));
    if (!put(line)) {
      ++truncated;
      break;
    }
  }

  for (const SuppressedIntervention& suppressed_entry : suppressed) {
    std::string line = "suppressed kind=" + std::string(to_string(suppressed_entry.kind));
    if (suppressed_entry.resource.valid()) {
      line.append(" resource=");
      line.append(to_string(suppressed_entry.resource));
    }
    line.append(" reason=");
    line.append(to_string(suppressed_entry.reason));
    if (!suppressed_entry.detail.empty()) {
      line.append(" detail=");
      line.append(suppressed_entry.detail);
    }
    if (!put(line)) {
      ++truncated;
      break;
    }
  }

  if (truncated > 0 || truncated_sections > 0) {
    // The truncation notice is never itself dropped: a bounded explanation that
    // silently looked complete would be worse than a truncated one.
    const std::string notice = "truncated-sections=" + number(truncated + truncated_sections) + "\n";
    if (out.size() + notice.size() <= max_bytes) {
      out.append(notice);
    } else if (max_bytes > notice.size()) {
      out.resize(max_bytes - notice.size());
      const std::size_t last_newline = out.rfind('\n');
      out.resize(last_newline == std::string::npos ? 0 : last_newline + 1);
      out.append(notice);
    }
  }
  return out;
}

std::string Explanation::summary() const {
  std::string out = to_string(domain);
  out.append(" state=");
  out.append(to_string(state));
  out.append(" severity=");
  out.append(to_string(severity));
  out.append(" authoritative=");
  out.append(authoritative ? "yes" : "no");
  out.append(" reason=");
  out.append(reason_code.empty() ? "unspecified" : reason_code);
  out.append(" authorized=");
  out.append(number(authorized.size()));
  out.append(" suppressed=");
  out.append(number(suppressed.size()));
  return out;
}

}  // namespace ncf
