////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2021 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Lars Maier
////////////////////////////////////////////////////////////////////////////////
#pragma once
#include "Replication2/ReplicatedLog/LogCommon.h"
#include "Replication2/ReplicatedState/AgencySpecification.h"
#include "Replication2/ReplicatedLog/SupervisionAction.h"
#include "Replication2/ReplicatedState/SupervisionAction.h"
#include "Replication2/ReplicatedLog/ParticipantsHealth.h"
#include "Replication2/Helper/ModelChecker/AgencyState.h"

namespace arangodb::test {

struct SupervisionStateAction {
  void apply(AgencyState& agency);

  explicit SupervisionStateAction(
      replication2::replicated_state::Action action);

  auto toString() const -> std::string;

  replication2::replicated_state::Action _action;
};

struct KillServerAction {
  void apply(AgencyState& agency);

  explicit KillServerAction(replication2::ParticipantId id);

  auto toString() const -> std::string;
  replication2::ParticipantId id;
};

struct SupervisionLogAction {
  void apply(AgencyState& agency);

  explicit SupervisionLogAction(replication2::replicated_log::Action action);
  auto toString() const -> std::string;

  replication2::replicated_log::Action _action;
};

struct DBServerSnapshotCompleteAction {
  explicit DBServerSnapshotCompleteAction(
      replication2::ParticipantId name,
      replication2::replicated_state::StateGeneration generation);

  void apply(AgencyState& agency);
  ;
  auto toString() const -> std::string;

  replication2::ParticipantId name;
  replication2::replicated_state::StateGeneration generation;
};

struct DBServerReportTermAction {
  explicit DBServerReportTermAction(replication2::ParticipantId name,
                                    replication2::LogTerm term);

  void apply(AgencyState& agency) const;
  auto toString() const -> std::string;

  replication2::ParticipantId name;
  replication2::LogTerm term;
};

struct DBServerCommitConfigAction {
  explicit DBServerCommitConfigAction(replication2::ParticipantId name,
                                      std::size_t generation,
                                      replication2::LogTerm term);

  void apply(AgencyState& agency) const;
  auto toString() const -> std::string;

  replication2::ParticipantId name;
  std::size_t generation;
  replication2::LogTerm term;
};

using AgencyTransition =
    std::variant<SupervisionStateAction, SupervisionLogAction,
                 DBServerSnapshotCompleteAction, DBServerReportTermAction,
                 DBServerCommitConfigAction, KillServerAction>;

auto operator<<(std::ostream& os, AgencyTransition const& a) -> std::ostream&;
}  // namespace arangodb::test
