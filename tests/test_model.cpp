// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"
#include "support.hpp"

#include <algorithm>
#include <string>

#include "ncf/core/crc32c.hpp"
#include "ncf/model/authority.hpp"
#include "ncf/model/enums.hpp"
#include "ncf/model/evidence.hpp"
#include "ncf/model/explanation.hpp"
#include "ncf/model/intervention.hpp"
#include "ncf/model/policy.hpp"
#include "ncf/model/topology.hpp"

using namespace ncf;
using namespace ncf::test;

NCF_TEST(metric_set_is_sorted_unique_and_bounded) {
  MetricSet set;
  NCF_CHECK(set.empty());
  NCF_CHECK(set.set(MetricKind::UtilizationPpm, 900000).ok());
  NCF_CHECK(set.set(MetricKind::LossRatePpm, 500).ok());
  NCF_CHECK(set.set(MetricKind::QueueDepthPackets, 42).ok());
  NCF_CHECK_EQ(set.size(), static_cast<std::size_t>(3));
  // The container is kept sorted by kind, so iteration order is canonical.
  NCF_CHECK(set.readings()[0].kind == MetricKind::UtilizationPpm);
  NCF_CHECK(set.readings()[1].kind == MetricKind::LossRatePpm);
  NCF_CHECK(set.readings()[2].kind == MetricKind::QueueDepthPackets);
  NCF_CHECK(!set.set(MetricKind::LossRatePpm, 1).ok());
  NCF_CHECK_EQ(set.get(MetricKind::LossRatePpm).value_or(0), 500ull);
  NCF_CHECK(!set.get(MetricKind::JitterMicros).has_value());
  NCF_CHECK(!set.set(MetricKind::Unknown, 1).ok());
}

NCF_TEST(metric_values_outside_their_range_are_refused) {
  MetricSet set;
  NCF_CHECK(!set.set(MetricKind::UtilizationPpm, 2000000).ok());
  NCF_CHECK(set.set(MetricKind::UtilizationPpm, 1500000).ok());
  NCF_CHECK(!set.set(MetricKind::QueueDepthPackets, 0xFFFFFFFFFFFFFFFFull).ok());
  const MetricValidation invalid = validate_metric_value(MetricKind::Unknown, 1);
  NCF_CHECK(!invalid.ok);
  NCF_CHECK(validate_metric_value(MetricKind::LatencyMicros, 1500).ok);
}

NCF_TEST(metric_descriptors_cover_the_whole_vocabulary) {
  for (std::uint16_t kind = 0; kind < kMetricKindCount; ++kind) {
    const MetricKind value = static_cast<MetricKind>(kind);
    if (value == MetricKind::Unknown || value == MetricKind::Count) {
      continue;
    }
    const std::string name(to_string(value));
    NCF_CHECK(!name.empty());
    MetricKind parsed = MetricKind::Unknown;
    NCF_CHECK(parse_metric_kind(name, parsed));
    NCF_CHECK(parsed == value);
  }
  MetricKind unparsed = MetricKind::Count;
  NCF_CHECK(!parse_metric_kind("not-a-metric", unparsed));
}

NCF_TEST(state_and_severity_helpers_are_consistent) {
  NCF_CHECK(is_authoritative(CongestionState::Clear));
  NCF_CHECK(is_authoritative(CongestionState::Recovering));
  NCF_CHECK(!is_authoritative(CongestionState::Stale));
  NCF_CHECK(is_indeterminate(CongestionState::Conflict));
  NCF_CHECK(!is_indeterminate(CongestionState::Severe));
  NCF_CHECK_EQ(severity_of(CongestionState::Severe).value(), Severity::Severe);
  NCF_CHECK(!severity_of(CongestionState::Unknown).has_value());
  NCF_CHECK(state_of(Severity::Watch) == CongestionState::Watch);
  NCF_CHECK(!higher_is_worse(MetricKind::ResidualCapacityBps));
  NCF_CHECK(higher_is_worse(MetricKind::LossRatePpm));
  NCF_CHECK(!higher_is_worse(MetricKind::DeliveryRatioPpm));
}

NCF_TEST(intervention_kinds_map_to_authority) {
  for (std::uint8_t kind = 1; kind < kInterventionKindCount; ++kind) {
    const InterventionKind value = static_cast<InterventionKind>(kind);
    NCF_CHECK(required_authority(value) != Authority::None);
    const std::string name(to_string(value));
    InterventionKind parsed = InterventionKind::None;
    NCF_CHECK(parse_intervention_kind(name, parsed));
    NCF_CHECK(parsed == value);
  }
  NCF_CHECK(required_authority(InterventionKind::None) == Authority::None);
}

NCF_TEST(authority_set_denial_beats_grant) {
  AuthorityVector vector;
  vector.epoch = EpochId::from_value(1);
  vector.generation = AuthorityGeneration::from_value(1);
  vector.policy = PolicyId::from_value(1);
  vector.policy_fingerprint = Hash64{1};
  vector.granted = AuthoritySet::all();
  vector.issued_tick = 10;

  NCF_CHECK(check_authority(vector, 10, Authority::Evaluate).allowed);
  NCF_CHECK(check_authority(vector, 10, Authority::RequestReroute).allowed);

  AuthorityVector denied = vector;
  denied.denied = AuthoritySet::of(Authority::RequestReroute);
  const AuthorityDecision decision = check_authority(denied, 10, Authority::RequestReroute);
  NCF_CHECK(!decision.allowed);
  NCF_CHECK(decision.reason == SuppressionReason::AuthorityDenied);

  AuthorityVector expired = vector;
  expired.expires_tick = 5;
  NCF_CHECK(!check_authority(expired, 10, Authority::Evaluate).allowed);
  NCF_CHECK(check_authority(expired, 10, Authority::Evaluate).reason == SuppressionReason::AuthorityExpired);

  AuthorityVector unbound = vector;
  unbound.epoch = EpochId{};
  NCF_CHECK(!check_authority(unbound, 10, Authority::Evaluate).allowed);
  NCF_CHECK(check_authority(unbound, 10, Authority::Evaluate).reason == SuppressionReason::EpochFenced);

  NCF_CHECK(std::string(AuthoritySet::none().describe()) == std::string("none"));
  NCF_CHECK(AuthoritySet::all().describe().find("evaluate") != std::string::npos);
}

NCF_TEST(default_policy_validates_and_is_stable) {
  CongestionPolicy policy = make_default_policy();
  const Hash64 first = fingerprint_policy(policy);
  const VoidResult valid = validate_policy(policy);
  NCF_CHECK(valid.ok());
  NCF_CHECK_EQ(policy.fingerprint.value, first.value);
  NCF_CHECK_EQ(fingerprint_policy(policy).value, first.value);
  NCF_CHECK_EQ(policy.rules.size(), static_cast<std::size_t>(4));
  NCF_CHECK(policy.min_agreeing_rules_for_congested >= 2);
}

NCF_TEST(policy_validation_rejects_broken_policies) {
  {
    CongestionPolicy policy = make_default_policy();
    policy.rules[0].watch = 900000;
    policy.rules[0].congested = 100000;
    NCF_CHECK(!validate_policy(policy).ok());
  }
  {
    CongestionPolicy policy = make_default_policy();
    policy.rules[0].severe = 2000000;
    NCF_CHECK(!validate_policy(policy).ok());
  }
  {
    CongestionPolicy policy = make_default_policy();
    policy.rules.push_back(policy.rules[0]);
    NCF_CHECK(!validate_policy(policy).ok());
  }
  {
    CongestionPolicy policy = make_default_policy();
    policy.recovery.allow_single_sample = false;
    policy.recovery.min_improved_samples = 1;
    NCF_CHECK(!validate_policy(policy).ok());
  }
  {
    CongestionPolicy policy = make_default_policy();
    policy.freshness.default_max_age_ticks = 0;
    NCF_CHECK(!validate_policy(policy).ok());
  }
  {
    CongestionPolicy policy = make_default_policy();
    policy.interventions[0].parameter_min = 100;
    policy.interventions[0].parameter_max = 10;
    NCF_CHECK(!validate_policy(policy).ok());
  }
  {
    CongestionPolicy policy = make_default_policy();
    policy.propagation.decay_ppm = 2000000;
    NCF_CHECK(!validate_policy(policy).ok());
  }
  {
    CongestionPolicy policy = make_default_policy();
    policy.min_agreeing_rules_for_severe = 1;
    policy.min_agreeing_rules_for_congested = 3;
    NCF_CHECK(!validate_policy(policy).ok());
  }
  {
    CongestionPolicy policy = make_default_policy();
    policy.id = PolicyId{};
    NCF_CHECK(!validate_policy(policy).ok());
  }
}

NCF_TEST(threshold_rule_classification_is_monotonic) {
  ThresholdRule rule;
  rule.metric = MetricKind::UtilizationPpm;
  rule.op = ThresholdOp::AtLeast;
  rule.watch = 700000;
  rule.congested = 850000;
  rule.severe = 950000;
  NCF_CHECK(rule.classify(0) == Severity::Clear);
  NCF_CHECK(rule.classify(699999) == Severity::Clear);
  NCF_CHECK(rule.classify(700000) == Severity::Watch);
  NCF_CHECK(rule.classify(850000) == Severity::Congested);
  NCF_CHECK(rule.classify(950000) == Severity::Severe);
  // The margin raises the classification, which is exactly what makes leaving a
  // level harder than entering it.
  NCF_CHECK(classify_rule_with_margin(rule, 830000, 50000) == Severity::Congested);
  NCF_CHECK(rule.classify(830000) == Severity::Watch);

  ThresholdRule residual;
  residual.metric = MetricKind::ResidualCapacityBps;
  residual.op = ThresholdOp::AtMost;
  residual.watch = 10000000000ull;
  residual.congested = 1000000000ull;
  residual.severe = 100000000ull;
  NCF_CHECK(residual.classify(20000000000ull) == Severity::Clear);
  NCF_CHECK(residual.classify(5000000000ull) == Severity::Watch);
  NCF_CHECK(residual.classify(500000000ull) == Severity::Congested);
  NCF_CHECK(residual.classify(1000ull) == Severity::Severe);
}

NCF_TEST(policy_freshness_override_is_per_metric) {
  CongestionPolicy policy = make_default_policy();
  policy.freshness.default_max_age_ticks = 1000;
  policy.rules[0].max_age_ticks = 5;
  NCF_CHECK_EQ(policy.max_age_for(policy.rules[0].metric), 5ull);
  NCF_CHECK_EQ(policy.max_age_for(MetricKind::JitterMicros), 1000ull);
}

NCF_TEST(topology_index_builds_and_refuses_dangling_edges) {
  TopologyBuilder builder;
  const DomainId domain = DomainId::from_value(1);
  const ResourceId first = builder.add_resource(domain);
  const ResourceId second = builder.add_resource(domain);
  builder.link(first, second);
  builder.add_path({first, second});
  const TopologySnapshot snapshot = builder.finish();

  Result<TopologyIndex> index = TopologyIndex::build(snapshot);
  NCF_REQUIRE(index.ok());
  NCF_CHECK_EQ(index.value().resource_count(), static_cast<std::size_t>(2));
  NCF_CHECK_EQ(index.value().domain_count(), static_cast<std::size_t>(1));
  NCF_CHECK(index.value().contains(first));
  NCF_CHECK(!index.value().contains(ResourceId::from_value(99)));
  NCF_CHECK_EQ(index.value().successors(first).size(), static_cast<std::size_t>(1));
  NCF_CHECK_EQ(index.value().predecessors(second).size(), static_cast<std::size_t>(1));
  NCF_CHECK_EQ(index.value().domain_resources(domain).size(), static_cast<std::size_t>(2));
  NCF_CHECK_EQ(index.value().resource_paths(first).size(), static_cast<std::size_t>(1));

  TopologySnapshot dangling = snapshot;
  dangling.links[0].to = ResourceId::from_value(777);
  NCF_CHECK(!TopologyIndex::build(dangling).ok());

  TopologySnapshot duplicated = snapshot;
  duplicated.resources.push_back(duplicated.resources[0]);
  NCF_CHECK(!TopologyIndex::build(duplicated).ok());

  TopologySnapshot empty;
  NCF_CHECK(!TopologyIndex::build(empty).ok());

  TopologySnapshot self_parent = snapshot;
  self_parent.resources[0].parent = self_parent.resources[0].id;
  NCF_CHECK(!TopologyIndex::build(self_parent).ok());
}

NCF_TEST(capacity_snapshot_validation) {
  CapacitySnapshot capacity;
  capacity.id = CapacitySnapshotId::from_value(1);
  capacity.generation = CapacityGeneration::from_value(1);
  capacity.entries.push_back(CapacityEntry{ResourceId::from_value(2), 100, 10, 10});
  capacity.entries.push_back(CapacityEntry{ResourceId::from_value(1), 100, 10, 10});
  NCF_CHECK(!validate_capacity(capacity).ok());
  std::reverse(capacity.entries.begin(), capacity.entries.end());
  NCF_CHECK(validate_capacity(capacity).ok());
  capacity.entries[0].residual_bps = 1000;
  NCF_CHECK(!validate_capacity(capacity).ok());
  NCF_CHECK(capacity.find(ResourceId::from_value(1)) != nullptr);
  NCF_CHECK(capacity.find(ResourceId::from_value(9)) == nullptr);
}

NCF_TEST(intervention_intent_validation_and_identity) {
  InterventionIntent intent;
  intent.kind = InterventionKind::ReduceAdmissibleBudget;
  intent.domain = DomainId::from_value(1);
  intent.basis_state = CongestionState::Congested;
  intent.basis_severity = Severity::Congested;
  intent.evidence = EvidenceSnapshotId::from_value(11);
  intent.evaluation = EvaluationId::from_value(12);
  intent.epoch = EpochId::from_value(1);
  intent.authority_generation = AuthorityGeneration::from_value(1);
  intent.policy = PolicyId::from_value(1);
  intent.policy_fingerprint = Hash64{7};
  intent.issued_tick = 100;
  NCF_CHECK(validate_intent(intent).ok());
  const InterventionId first = compute_intervention_id(intent);
  NCF_CHECK(first.valid());
  NCF_CHECK_EQ(compute_intervention_id(intent).value(), first.value());

  InterventionIntent non_authoritative = intent;
  non_authoritative.basis_state = CongestionState::Unknown;
  NCF_CHECK(!validate_intent(non_authoritative).ok());

  InterventionIntent unbounded = intent;
  unbounded.evidence = EvidenceSnapshotId{};
  NCF_CHECK(!validate_intent(unbounded).ok());

  InterventionIntent backwards = intent;
  backwards.expires_tick = 5;
  NCF_CHECK(!validate_intent(backwards).ok());

  InterventionIntent long_rationale = intent;
  long_rationale.rationale = std::string(600, 'x');
  NCF_CHECK(!validate_intent(long_rationale).ok());
}

NCF_TEST(explanation_render_is_bounded_and_complete) {
  Explanation explanation;
  explanation.domain = DomainId::from_value(1);
  explanation.state = CongestionState::Congested;
  explanation.previous_state = CongestionState::Watch;
  explanation.severity = Severity::Congested;
  explanation.transitioned = true;
  explanation.authoritative = true;
  explanation.evaluation = EvaluationId::from_value(9);
  explanation.epoch = EpochId::from_value(2);
  explanation.policy = PolicyId::from_value(3);
  explanation.policy_fingerprint = Hash64{1234};
  explanation.authority = AuthoritySet::all();
  explanation.evaluated_tick = 500;
  explanation.reason_code = "severity-escalated";
  ThresholdBinding binding;
  binding.resource = ResourceId::from_value(1);
  binding.metric = MetricKind::UtilizationPpm;
  binding.value = 990000;
  binding.watch = 700000;
  binding.congested = 850000;
  binding.severe = 950000;
  binding.level = Severity::Severe;
  explanation.bindings.push_back(binding);
  for (std::uint64_t index = 0; index < 500; ++index) {
    ResourceAssessment assessment;
    assessment.resource = ResourceId::from_value(index + 1);
    explanation.resources.push_back(assessment);
  }
  const std::string rendered = explanation.render();
  NCF_CHECK(rendered.find("state=congested") != std::string::npos);
  NCF_CHECK(rendered.find("reason=severity-escalated") != std::string::npos);
  NCF_CHECK(rendered.find("binding resource=resource:1") != std::string::npos);
  const std::string tiny = explanation.render(200);
  NCF_CHECK(tiny.size() <= 200);
  NCF_CHECK(tiny.find("truncated-sections=") != std::string::npos);
  NCF_CHECK(!explanation.summary().empty());
}

NCF_TEST(evidence_snapshot_identity_is_content_bound) {
  const EvidenceSample base = make_sample(ResourceId::from_value(1), PublisherId::from_value(1), 100,
                                          {{MetricKind::UtilizationPpm, 900000}});
  EvidenceSample same = base;
  NCF_CHECK_EQ(compute_snapshot_id(same).value(), base.snapshot.value());
  same.metrics.clear();
  (void)same.metrics.set(MetricKind::UtilizationPpm, 900001);
  NCF_CHECK_NE(compute_snapshot_id(same).value(), base.snapshot.value());
  same = base;
  same.observed_tick = 101;
  NCF_CHECK_NE(compute_snapshot_id(same).value(), base.snapshot.value());
}
