// Network Congestion Fabric
// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"
#include "support.hpp"

#include <string>
#include <vector>

#include "ncf/eval/hysteresis.hpp"
#include "ncf/eval/propagation.hpp"
#include "ncf/model/evaluation.hpp"

using namespace ncf;
using namespace ncf::test;

namespace {

const DomainId kDomain = DomainId::from_value(1);
const PublisherId kPublisher = PublisherId::from_value(1);

struct SingleResource {
  TopologySnapshot topology;
  ResourceId resource;
};

[[nodiscard]] SingleResource one_resource() {
  TopologyBuilder builder;
  const ResourceId resource = builder.add_resource(kDomain);
  return SingleResource{builder.finish(), resource};
}

}  // namespace

NCF_TEST(evaluation_without_policy_is_not_ready) {
  EvaluationHarness harness;
  EvaluationContext context;
  context.policy = nullptr;
  EvaluationRequest request;
  request.domain = kDomain;
  request.now = 100;
  const Result<EvaluationOutcome> outcome = CongestionEvaluator::evaluate(request, DomainState{}, context);
  NCF_CHECK(!outcome.ok());
  NCF_CHECK(outcome.status().code() == ErrCode::NotReady);
}

NCF_TEST(evaluation_requires_a_domain_and_a_tick) {
  EvaluationHarness harness;
  EvaluationContext context = harness.context();
  EvaluationRequest request;
  request.domain = DomainId{};
  request.now = 100;
  NCF_CHECK(CongestionEvaluator::evaluate(request, DomainState{}, context).status().code() ==
            ErrCode::InvalidArgument);
  request.domain = kDomain;
  request.now = kNoTick;
  NCF_CHECK(CongestionEvaluator::evaluate(request, DomainState{}, context).status().code() ==
            ErrCode::InvalidArgument);
}

NCF_TEST(high_utilization_alone_is_not_congestion) {
  // The modelling rule under test: a single metric can raise a domain to WATCH,
  // which carries no corrective intervention, but never to CONGESTED.
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  const std::vector<EvidenceSample> samples = {make_sample(
      fixture.resource, kPublisher, harness.clock().now(), {{MetricKind::UtilizationPpm, 999000}})};
  const Result<EvaluationOutcome> outcome = harness.repeat(kDomain, samples, 4);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Watch);
  NCF_CHECK(outcome.value().authoritative);
  NCF_CHECK_EQ(outcome.value().evidence.bindings.size(), static_cast<std::size_t>(1));
  // The binding records what the rule itself saw; the gate is what keeps the
  // domain at WATCH, and the gate is reported separately.
  NCF_CHECK(outcome.value().evidence.bindings[0].level == Severity::Severe);
  bool saw_gate = false;
  for (const std::string& detail : outcome.value().reason_detail) {
    if (detail.find("corroboration gate") != std::string::npos) {
      saw_gate = true;
    }
  }
  NCF_CHECK(saw_gate);
}

NCF_TEST(queue_occupancy_alone_is_not_congestion) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  const std::vector<EvidenceSample> samples = {make_sample(
      fixture.resource, kPublisher, harness.clock().now(), {{MetricKind::QueueOccupancyPpm, 990000}})};
  const Result<EvaluationOutcome> outcome = harness.repeat(kDomain, samples, 4);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Watch);
}

NCF_TEST(loss_alone_is_not_congestion) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  const std::vector<EvidenceSample> samples = {
      make_sample(fixture.resource, kPublisher, harness.clock().now(), {{MetricKind::LossRatePpm, 900000}})};
  const Result<EvaluationOutcome> outcome = harness.repeat(kDomain, samples, 4);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Watch);
}

NCF_TEST(corroborated_congestion_escalates_deterministically) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  const std::vector<EvidenceSample> samples =
      saturated_resources({fixture.resource}, kPublisher, harness.clock().now());
  const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Severe);
  NCF_CHECK(outcome.value().transitioned);
  NCF_CHECK(outcome.value().authoritative);
  NCF_CHECK_EQ(outcome.value().reason_code, std::string("severity-escalated"));
  NCF_CHECK_EQ(outcome.value().evidence.bindings.size(), static_cast<std::size_t>(4));
}

NCF_TEST(severity_transitions_are_deterministic_across_replays) {
  const SingleResource fixture = one_resource();
  std::vector<CongestionState> first_run;
  std::vector<CongestionState> second_run;
  const std::vector<std::uint64_t> utilization = {100000, 720000, 720000, 880000, 880000,
                                                  990000, 100000, 100000, 100000};
  for (int pass = 0; pass < 2; ++pass) {
    EvaluationHarness harness;
    harness.set_topology(fixture.topology);
    std::vector<CongestionState>& trace = pass == 0 ? first_run : second_run;
    for (const std::uint64_t value : utilization) {
      const std::vector<EvidenceSample> samples = {
          make_sample(fixture.resource, kPublisher, harness.clock().now(),
                      {{MetricKind::UtilizationPpm, value},
                       {MetricKind::QueueOccupancyPpm, value > 800000 ? 600000 : 10000},
                       {MetricKind::LossRatePpm, value > 800000 ? 30000 : 100}}),
          make_sample(fixture.resource, PublisherId::from_value(2), harness.clock().now(),
                      {{MetricKind::UtilizationPpm, value},
                       {MetricKind::QueueOccupancyPpm, value > 800000 ? 600000 : 10000},
                       {MetricKind::LossRatePpm, value > 800000 ? 30000 : 100}},
                      EpochId::from_value(1), 2)};
      const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
      NCF_REQUIRE(outcome.ok());
      trace.push_back(outcome.value().state);
      harness.advance(1000);
    }
  }
  NCF_CHECK_EQ(first_run.size(), second_run.size());
  for (std::size_t index = 0; index < first_run.size(); ++index) {
    NCF_CHECK(first_run[index] == second_run[index]);
  }
}

NCF_TEST(hysteresis_requires_repeated_improvement_before_recovery) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  Result<EvaluationOutcome> outcome =
      harness.step(kDomain, saturated_resources({fixture.resource}, kPublisher, harness.clock().now()));
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Severe);

  harness.advance(1000);
  const std::vector<EvidenceSample> improved = idle_resources({fixture.resource}, kPublisher, harness.clock().now());
  outcome = harness.step(kDomain, improved);
  NCF_REQUIRE(outcome.ok());
  // One improved sample cannot start recovery.
  NCF_CHECK(outcome.value().state == CongestionState::Severe);
  NCF_CHECK_EQ(outcome.value().reason_code, std::string("hysteresis-pending"));

  harness.advance(1000);
  outcome = harness.step(kDomain, improved);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Recovering);
  NCF_CHECK_EQ(outcome.value().reason_code, std::string("recovery-started"));

  harness.advance(1000);
  outcome = harness.step(kDomain, improved);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Recovering);
}

NCF_TEST(single_sample_recovery_only_when_policy_permits_it) {
  const SingleResource fixture = one_resource();
  CongestionPolicy policy = make_default_policy();
  policy.recovery.allow_single_sample = true;
  policy.recovery.min_improved_samples = 1;
  EvaluationHarness harness(policy);
  harness.set_topology(fixture.topology);

  Result<EvaluationOutcome> outcome =
      harness.step(kDomain, saturated_resources({fixture.resource}, kPublisher, harness.clock().now()));
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Severe);
  harness.advance(1000);
  outcome = harness.step(kDomain, idle_resources({fixture.resource}, kPublisher, harness.clock().now()));
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Recovering);
}

NCF_TEST(downgrade_margin_must_be_cleared) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  const auto congested = [&](std::uint64_t utilization) {
    return std::vector<EvidenceSample>{
        make_sample(fixture.resource, kPublisher, harness.clock().now(),
                    {{MetricKind::UtilizationPpm, utilization},
                     {MetricKind::QueueOccupancyPpm, 600000},
                     {MetricKind::LossRatePpm, 30000}})};
  };
  Result<EvaluationOutcome> outcome = harness.step(kDomain, congested(900000));
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Congested);

  // 830000 is below the congestion threshold but inside the 5 percent margin, so
  // the domain must not leave CONGESTED.
  for (int index = 0; index < 6; ++index) {
    harness.advance(1000);
    outcome = harness.step(kDomain, congested(830000));
    NCF_REQUIRE(outcome.ok());
  }
  NCF_CHECK(outcome.value().state == CongestionState::Congested);

  // Utilisation alone dropping below the margin is not enough while queue and
  // loss still read congested.
  for (int index = 0; index < 6; ++index) {
    harness.advance(1000);
    outcome = harness.step(kDomain, congested(700000));
    NCF_REQUIRE(outcome.ok());
  }
  NCF_CHECK(outcome.value().state == CongestionState::Congested);

  const auto all_calm = [&]() {
    return std::vector<EvidenceSample>{
        make_sample(fixture.resource, kPublisher, harness.clock().now(),
                    {{MetricKind::UtilizationPpm, 300000},
                     {MetricKind::QueueOccupancyPpm, 1000},
                     {MetricKind::LossRatePpm, 0}})};
  };
  for (int index = 0; index < 6; ++index) {
    harness.advance(1000);
    outcome = harness.step(kDomain, all_calm());
    NCF_REQUIRE(outcome.ok());
  }
  NCF_CHECK(outcome.value().state == CongestionState::Recovering ||
            outcome.value().state == CongestionState::Watch || outcome.value().state == CongestionState::Clear);
}

NCF_TEST(min_dwell_prevents_oscillation) {
  const SingleResource fixture = one_resource();
  CongestionPolicy policy = make_default_policy();
  policy.hysteresis.min_dwell_ticks = 10000;
  policy.hysteresis.escalate_samples = 1;
  policy.hysteresis.deescalate_samples = 1;
  policy.hysteresis.apply_dwell_on_escalation = true;
  policy.recovery.enabled = false;
  EvaluationHarness harness(policy);
  harness.set_topology(fixture.topology);

  const auto severity_samples = [&](std::uint64_t utilization) {
    return std::vector<EvidenceSample>{
        make_sample(fixture.resource, kPublisher, harness.clock().now(),
                    {{MetricKind::UtilizationPpm, utilization},
                     {MetricKind::QueueOccupancyPpm, utilization > 800000 ? 600000 : 1000},
                     {MetricKind::LossRatePpm, utilization > 800000 ? 30000 : 0}})};
  };

  Result<EvaluationOutcome> outcome = harness.step(kDomain, severity_samples(100000));
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Clear);

  harness.advance(1000);
  outcome = harness.step(kDomain, severity_samples(990000));
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Clear);
  NCF_CHECK_EQ(outcome.value().reason_code, std::string("hysteresis-hold"));

  harness.advance(20000);
  outcome = harness.step(kDomain, severity_samples(990000));
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Congested);
}

NCF_TEST(stale_evidence_never_authorizes_and_demotes_to_stale) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  Result<EvaluationOutcome> outcome =
      harness.step(kDomain, saturated_resources({fixture.resource}, kPublisher, harness.clock().now()));
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Severe);

  // Move the clock beyond the freshness bound without providing new evidence.
  harness.advance(2000000);
  EvaluationRequest request = [&]() {
    EvaluationRequest built;
    built.domain = kDomain;
    built.epoch = EpochId::from_value(1);
    built.now = harness.clock().now();
    built.evidence = make_batch({}, EpochId::from_value(1), harness.clock().now());
    return built;
  }();
  EvaluationContext context = harness.context();
  request.authority = context.authority;
  outcome = CongestionEvaluator::evaluate(request, harness.prior(), context);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Stale);
  NCF_CHECK(!outcome.value().authoritative);
  NCF_CHECK(!is_authoritative(outcome.value().state));
}

NCF_TEST(a_stale_sample_is_refused_and_reported) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  harness.clock().set(10000000);
  const Tick now = harness.clock().now();
  const std::vector<EvidenceSample> fresh = saturated_resources({fixture.resource}, kPublisher, now);
  const std::vector<EvidenceSample> old = saturated_resources({fixture.resource}, kPublisher, now - 5000000);
  std::vector<EvidenceSample> mixed = fresh;
  mixed.insert(mixed.end(), old.begin(), old.end());
  const Result<EvaluationOutcome> outcome = harness.step(kDomain, mixed);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK_EQ(outcome.value().evidence.stale.size(), static_cast<std::size_t>(4));
  NCF_CHECK_EQ(outcome.value().evidence.samples_accepted, static_cast<std::size_t>(1));
  NCF_CHECK_EQ(outcome.value().evidence.samples_rejected, static_cast<std::size_t>(0));
}

NCF_TEST(future_evidence_beyond_the_skew_bound_is_refused) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  const Tick now = harness.clock().now();
  // Inside the skew bound: accepted but recorded as future.
  std::vector<EvidenceSample> samples =
      saturated_resources({fixture.resource}, kPublisher, now + 500);
  Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().evidence.future_samples >= 1);
  NCF_CHECK_EQ(outcome.value().evidence.samples_accepted, samples.size());
  NCF_CHECK(!outcome.value().evidence.resources.empty());
  NCF_CHECK(outcome.value().evidence.resources[0].future_samples >= 1);

  EvaluationHarness other;
  other.set_topology(fixture.topology);
  samples = saturated_resources({fixture.resource}, kPublisher, other.clock().now() + 500000);
  outcome = other.step(kDomain, samples);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK_EQ(outcome.value().evidence.samples_accepted, static_cast<std::size_t>(0));
  NCF_CHECK(!outcome.value().evidence.stale.empty());
  NCF_CHECK(outcome.value().evidence.stale[0].beyond_future_skew);
  NCF_CHECK(outcome.value().state == CongestionState::Unknown);
}

NCF_TEST(contradictory_publishers_produce_conflict_not_a_merge) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  const Tick now = harness.clock().now();
  std::vector<EvidenceSample> samples =
      saturated_resources({fixture.resource}, PublisherId::from_value(1), now, 990000, 900000, 300000, 60000);
  const std::vector<EvidenceSample> calm =
      idle_resources({fixture.resource}, PublisherId::from_value(2), now);
  samples.insert(samples.end(), calm.begin(), calm.end());
  const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Conflict);
  NCF_CHECK(!outcome.value().authoritative);
  NCF_CHECK_EQ(outcome.value().reason_code, std::string("contradictory-evidence"));
  NCF_CHECK(!outcome.value().evidence.conflicts.empty());
  NCF_CHECK(!outcome.value().evidence.conflicts[0].resolved);
}

NCF_TEST(contradiction_resolution_modes_still_report_the_disagreement) {
  const SingleResource fixture = one_resource();
  CongestionPolicy policy = make_default_policy();
  policy.contradiction.mode = ContradictionPolicy::Mode::PreferNewest;
  EvaluationHarness harness(policy);
  harness.set_topology(fixture.topology);
  const Tick now = harness.clock().now();
  std::vector<EvidenceSample> samples =
      saturated_resources({fixture.resource}, PublisherId::from_value(1), now - 1000, 990000, 900000, 300000, 60000);
  const std::vector<EvidenceSample> calm = idle_resources({fixture.resource}, PublisherId::from_value(2), now);
  samples.insert(samples.end(), calm.begin(), calm.end());
  const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state != CongestionState::Conflict);
  NCF_CHECK(!outcome.value().evidence.conflicts.empty());
  NCF_CHECK(outcome.value().evidence.conflicts[0].resolved);
  NCF_CHECK(outcome.value().evidence.conflicts[0].selected_publisher.valid());
}

NCF_TEST(unknown_domain_and_missing_resources_are_reported) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  Result<EvaluationOutcome> outcome = harness.step(DomainId::from_value(42), {});
  NCF_CHECK(!outcome.ok());
  NCF_CHECK(outcome.status().code() == ErrCode::NotFound);

  EvaluationRequest request;
  request.domain = kDomain;
  request.now = harness.clock().now();
  request.only_resource = ResourceId::from_value(999);
  request.evidence = make_batch({}, EpochId::from_value(1), harness.clock().now());
  request.authority = harness.context().authority;
  NCF_CHECK(CongestionEvaluator::evaluate(request, DomainState{}, harness.context()).status().code() ==
            ErrCode::NotFound);
}

NCF_TEST(epoch_mismatched_samples_are_fenced) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  std::vector<EvidenceSample> samples = saturated_resources({fixture.resource}, kPublisher, harness.clock().now());
  for (EvidenceSample& sample : samples) {
    sample.epoch = EpochId::from_value(99);
    sample.snapshot = compute_snapshot_id(sample);
  }
  const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK_EQ(outcome.value().evidence.samples_accepted, static_cast<std::size_t>(0));
  NCF_CHECK_EQ(outcome.value().evidence.samples_rejected, samples.size());
  NCF_CHECK(outcome.value().state == CongestionState::Unknown);
}

NCF_TEST(duplicate_samples_are_counted_once) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  std::vector<EvidenceSample> samples = saturated_resources({fixture.resource}, kPublisher, harness.clock().now());
  const std::vector<EvidenceSample> copy = samples;
  samples.insert(samples.end(), copy.begin(), copy.end());
  const Result<EvaluationOutcome> outcome = harness.step(kDomain, samples);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK_EQ(outcome.value().evidence.samples_duplicate, copy.size());
}

NCF_TEST(partial_coverage_cannot_claim_health) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  // Utilization is required by the default policy; a sample that omits it leaves
  // the resource only partially covered, and partial coverage must not be read
  // as improvement.
  const std::vector<EvidenceSample> samples = {
      make_sample(fixture.resource, kPublisher, harness.clock().now(), {{MetricKind::LatencyMicros, 10}})};
  const Result<EvaluationOutcome> outcome = harness.repeat(kDomain, samples, 5);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Watch);
  NCF_CHECK(outcome.value().evidence.resources[0].coverage == ResourceCoverage::Partial);
}

NCF_TEST(heartbeats_do_not_create_state) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  const std::vector<EvidenceSample> samples = {make_sample(fixture.resource, kPublisher, harness.clock().now(),
                                                           {}, EpochId::from_value(1), 1, QueueId{},
                                                           kEvidenceFlagHeartbeat)};
  const Result<EvaluationOutcome> outcome = harness.repeat(kDomain, samples, 3);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Unknown);
  NCF_CHECK_EQ(outcome.value().evidence.heartbeats, static_cast<std::size_t>(1));
}

NCF_TEST(tick_regression_is_refused) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  Result<EvaluationOutcome> outcome =
      harness.step(kDomain, saturated_resources({fixture.resource}, kPublisher, harness.clock().now()));
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Severe);
  // Rewind the clock and re-evaluate; the machine must refuse to move.
  harness.clock().set(1);
  EvaluationRequest request = [&]() {
    EvaluationRequest built;
    built.domain = kDomain;
    built.epoch = EpochId::from_value(1);
    built.now = 1;
    built.evidence =
        make_batch(saturated_resources({fixture.resource}, kPublisher, 1), EpochId::from_value(1), 1);
    return built;
  }();
  EvaluationContext context = harness.context();
  request.authority = context.authority;
  outcome = CongestionEvaluator::evaluate(request, harness.prior(), context);
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK_EQ(outcome.value().reason_code, std::string("tick-regression"));
  NCF_CHECK(outcome.value().state == CongestionState::Severe);
}

NCF_TEST(intervention_planning_is_bounded_and_authority_checked) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  const Result<EvaluationOutcome> outcome =
      harness.step(kDomain, saturated_resources({fixture.resource}, kPublisher, harness.clock().now()));
  NCF_REQUIRE(outcome.ok());

  PlanningContext context;
  context.policy = &harness.policy();
  context.now = harness.clock().now();
  context.epoch = EpochId::from_value(1);
  context.authority = harness.context().authority;

  Result<InterventionPlan> plan = InterventionPlanner::plan(outcome.value(), context, {});
  NCF_REQUIRE(plan.ok());
  NCF_CHECK(!plan.value().authorized.empty());
  for (const InterventionIntent& intent : plan.value().authorized) {
    NCF_CHECK(validate_intent(intent).ok());
    NCF_CHECK(intent.bound_to(context.authority));
    NCF_CHECK(!intent.parameter_clamped);
  }

  PlanningContext denied = context;
  denied.authority.denied = AuthoritySet::all();
  plan = InterventionPlanner::plan(outcome.value(), denied, {});
  NCF_REQUIRE(plan.ok());
  NCF_CHECK(plan.value().authorized.empty());
  NCF_CHECK(!plan.value().suppressed.empty());
  NCF_CHECK(plan.value().suppressed[0].reason == SuppressionReason::AuthorityDenied);
}

NCF_TEST(unknown_state_authorizes_nothing) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  const Result<EvaluationOutcome> outcome = harness.step(kDomain, {});
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().state == CongestionState::Unknown);

  PlanningContext context;
  context.policy = &harness.policy();
  context.now = harness.clock().now();
  context.epoch = EpochId::from_value(1);
  context.authority = harness.context().authority;
  const Result<InterventionPlan> plan = InterventionPlanner::plan(outcome.value(), context, {});
  NCF_REQUIRE(plan.ok());
  NCF_CHECK(plan.value().authorized.empty());
  bool saw_non_authoritative = false;
  for (const SuppressedIntervention& suppressed : plan.value().suppressed) {
    if (suppressed.reason == SuppressionReason::NonAuthoritativeState) {
      saw_non_authoritative = true;
    }
  }
  NCF_CHECK(saw_non_authoritative);
}

NCF_TEST(already_outstanding_intent_is_suppressed) {
  const SingleResource fixture = one_resource();
  EvaluationHarness harness;
  harness.set_topology(fixture.topology);
  const Result<EvaluationOutcome> outcome =
      harness.step(kDomain, saturated_resources({fixture.resource}, kPublisher, harness.clock().now()));
  NCF_REQUIRE(outcome.ok());
  PlanningContext context;
  context.policy = &harness.policy();
  context.now = harness.clock().now();
  context.epoch = EpochId::from_value(1);
  context.authority = harness.context().authority;
  Result<InterventionPlan> plan = InterventionPlanner::plan(outcome.value(), context, {});
  NCF_REQUIRE(plan.ok());
  const std::vector<InterventionIntent> active = plan.value().authorized;
  NCF_CHECK(!active.empty());
  plan = InterventionPlanner::plan(outcome.value(), context, active);
  NCF_REQUIRE(plan.ok());
  NCF_CHECK(plan.value().authorized.empty());
}

NCF_TEST(propagation_never_exceeds_the_policy_ceiling) {
  TopologyBuilder builder;
  const ResourceId first = builder.add_resource(kDomain);
  const ResourceId second = builder.add_resource(kDomain);
  const ResourceId third = builder.add_resource(kDomain);
  builder.link(first, second);
  builder.link(second, third);
  const TopologySnapshot topology = builder.finish();
  Result<TopologyIndex> index = TopologyIndex::build(topology);
  NCF_REQUIRE(index.ok());

  PropagationPolicy policy;
  policy.enabled = true;
  policy.max_depth = 1;
  policy.decay_ppm = 900000;
  policy.max_induced_severity = Severity::Watch;
  const std::vector<PropagationEngine::Source> sources = {{first, Severity::Severe}};
  const Result<PropagationResult> result = PropagationEngine::propagate(index.value(), sources, policy, 64);
  NCF_REQUIRE(result.ok());
  NCF_CHECK_EQ(result.value().induced.size(), static_cast<std::size_t>(1));
  NCF_CHECK(result.value().induced[0].first == second);
  NCF_CHECK(result.value().induced[0].second == Severity::Watch);
  NCF_CHECK_EQ(result.value().edges.size(), static_cast<std::size_t>(1));
  NCF_CHECK_EQ(result.value().edges[0].hops, 1u);
}

NCF_TEST(propagation_depth_and_decay_are_bounded) {
  TopologyBuilder builder;
  const ResourceId first = builder.add_resource(kDomain);
  const ResourceId second = builder.add_resource(kDomain);
  const ResourceId third = builder.add_resource(kDomain);
  builder.link(first, second);
  builder.link(second, third);
  Result<TopologyIndex> index = TopologyIndex::build(builder.finish());
  NCF_REQUIRE(index.ok());

  PropagationPolicy policy;
  policy.max_depth = 2;
  policy.decay_ppm = 900000;
  policy.max_induced_severity = Severity::Congested;
  const std::vector<PropagationEngine::Source> sources = {{first, Severity::Severe}};
  const Result<PropagationResult> result = PropagationEngine::propagate(index.value(), sources, policy, 64);
  NCF_REQUIRE(result.ok());
  NCF_CHECK_EQ(result.value().induced.size(), static_cast<std::size_t>(2));
  NCF_CHECK(result.value().induced[0].first == second);
  NCF_CHECK(result.value().induced[1].first == third);
  NCF_CHECK(result.value().induced[1].second <= Severity::Congested);

  const Result<PropagationResult> bounded = PropagationEngine::propagate(index.value(), sources, policy, 1);
  NCF_REQUIRE(bounded.ok());
  NCF_CHECK(bounded.value().truncated);

  const Result<PropagationResult> refused =
      PropagationEngine::propagate(index.value(), sources, policy, limits::kMaxPropagationEdges + 1);
  NCF_CHECK(!refused.ok());
}

NCF_TEST(propagation_can_follow_paths_not_only_links) {
  TopologyBuilder builder;
  const ResourceId first = builder.add_resource(kDomain);
  const ResourceId second = builder.add_resource(kDomain);
  builder.add_path({first, second});
  Result<TopologyIndex> index = TopologyIndex::build(builder.finish());
  NCF_REQUIRE(index.ok());

  PropagationPolicy policy;
  policy.max_depth = 1;
  policy.follow_links = false;
  policy.follow_paths = true;
  policy.decay_ppm = 900000;
  policy.max_induced_severity = Severity::Watch;
  const std::vector<PropagationEngine::Source> sources = {{first, Severity::Severe}};
  const Result<PropagationResult> result = PropagationEngine::propagate(index.value(), sources, policy, 64);
  NCF_REQUIRE(result.ok());
  NCF_CHECK_EQ(result.value().induced.size(), static_cast<std::size_t>(1));
  NCF_CHECK(result.value().edges[0].via_path.valid());
}

NCF_TEST(propagation_is_disabled_by_policy) {
  TopologyBuilder builder;
  const ResourceId first = builder.add_resource(kDomain);
  const ResourceId second = builder.add_resource(kDomain);
  builder.link(first, second);
  Result<TopologyIndex> index = TopologyIndex::build(builder.finish());
  NCF_REQUIRE(index.ok());
  PropagationPolicy policy;
  policy.enabled = false;
  const std::vector<PropagationEngine::Source> sources = {{first, Severity::Severe}};
  const Result<PropagationResult> result = PropagationEngine::propagate(index.value(), sources, policy, 64);
  NCF_REQUIRE(result.ok());
  NCF_CHECK(result.value().induced.empty());
  NCF_CHECK(result.value().edges.empty());
}

NCF_TEST(oversized_domain_is_refused_rather_than_truncated) {
  TopologyBuilder builder;
  for (int index = 0; index < 8; ++index) {
    builder.add_resource(kDomain);
  }
  Result<TopologyIndex> index = TopologyIndex::build(builder.finish());
  NCF_REQUIRE(index.ok());
  CongestionPolicy policy = make_default_policy();
  policy.limits.max_resources_per_domain = 4;
  EvaluationHarness harness(policy);
  harness.set_topology(index.value().snapshot());
  const Result<EvaluationOutcome> outcome = harness.step(kDomain, {});
  NCF_CHECK(!outcome.ok());
  NCF_CHECK(outcome.status().code() == ErrCode::LimitExceeded);
}

NCF_TEST(oversized_evidence_batch_is_refused) {
  EvaluationHarness harness;
  EvaluationContext context = harness.context();
  context.max_samples_per_batch = 2;
  EvaluationRequest request;
  request.domain = kDomain;
  request.now = harness.clock().now();
  request.evidence = make_batch({make_sample(ResourceId::from_value(1), kPublisher, 100, {}),
                                 make_sample(ResourceId::from_value(1), kPublisher, 100, {}),
                                 make_sample(ResourceId::from_value(1), kPublisher, 100, {})});
  NCF_CHECK(CongestionEvaluator::evaluate(request, DomainState{}, context).status().code() ==
            ErrCode::LimitExceeded);
}

NCF_TEST(explanation_bounds_are_respected_for_large_domains) {
  TopologyBuilder builder;
  std::vector<ResourceId> resources;
  for (int index = 0; index < 32; ++index) {
    resources.push_back(builder.add_resource(kDomain));
  }
  EvaluationHarness harness;
  harness.set_topology(builder.finish());
  harness.policy().limits.max_explanation_entries = 4;
  const Result<EvaluationOutcome> outcome =
      harness.step(kDomain, saturated_resources(resources, kPublisher, harness.clock().now()));
  NCF_REQUIRE(outcome.ok());
  NCF_CHECK(outcome.value().evidence.bindings.size() <= 4);
  NCF_CHECK(outcome.value().evidence.truncated_sections > 0);
}

NCF_TEST(evidence_freshness_helper_matches_the_bound) {
  EvidenceVector vector;
  CongestionPolicy policy = make_default_policy();
  NCF_CHECK(!evidence_within_freshness(vector, policy, 1000));
  vector.samples_accepted = 1;
  vector.newest_observed_tick = 1000;
  NCF_CHECK(evidence_within_freshness(vector, policy, 1000));
  NCF_CHECK(evidence_within_freshness(vector, policy, 1000 + policy.freshness.default_max_age_ticks));
  NCF_CHECK(!evidence_within_freshness(vector, policy,
                                       1001 + policy.freshness.default_max_age_ticks));
  vector.newest_observed_tick = 2000;
  NCF_CHECK(evidence_within_freshness(vector, policy, 1000));
  vector.newest_observed_tick = 1000 + policy.freshness.max_future_skew_ticks + 1;
  NCF_CHECK(!evidence_within_freshness(vector, policy, 1000));
}

NCF_TEST(restart_demotion_never_restores_authority) {
  CongestionPolicy policy = make_default_policy();
  bool requires_revalidation = false;
  const CongestionState demoted = demote_after_recovery(CongestionState::Severe, 1000, policy, 1000,
                                                        requires_revalidation);
  NCF_CHECK(demoted == CongestionState::Stale);
  NCF_CHECK(requires_revalidation);
  requires_revalidation = false;
  NCF_CHECK(demote_after_recovery(CongestionState::Unknown, 0, policy, 1000, requires_revalidation) ==
            CongestionState::Unknown);
  NCF_CHECK(requires_revalidation);
  NCF_CHECK(demote_after_recovery(CongestionState::Clear, 0, policy, 1000, requires_revalidation) ==
            CongestionState::Unknown);
  // A tick recorded by a previous process incarnation is not comparable with the
  // recovered clock, so a restart never produces CONFLICT.
  NCF_CHECK(demote_after_recovery(CongestionState::Watch, 5000, policy, 1000, requires_revalidation) ==
            CongestionState::Stale);
  NCF_CHECK(demote_after_recovery(CongestionState::Conflict, 5000, policy, 1000, requires_revalidation) ==
            CongestionState::Conflict);
  NCF_CHECK(demote_after_recovery(CongestionState::Stale, 5000, policy, 1000, requires_revalidation) ==
            CongestionState::Stale);
}

NCF_TEST(hysteresis_engine_refuses_an_inconsistent_proposal) {
  DomainState state;
  SeverityProposal proposal;
  proposal.has_value = false;
  proposal.indeterminate = CongestionState::Congested;
  const HysteresisStep step =
      HysteresisEngine::step(state, proposal, HysteresisPolicy{}, RecoveryPolicy{}, 100);
  NCF_CHECK(std::string(step.reason) == std::string("invalid-proposal"));
  NCF_CHECK(!step.transitioned);
}
