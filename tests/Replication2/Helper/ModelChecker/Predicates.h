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
#include "Replication2/Helper/ModelChecker/AgencyState.h"
#include "Replication2/Helper/ModelChecker/HashValues.h"
#include "Replication2/ModelChecker/ModelChecker.h"
#include "Replication2/ModelChecker/ActorModel.h"
#include "Replication2/ModelChecker/Predicates.h"

namespace arangodb::test::mcpreds {
static inline auto isLeaderHealth() {
  return MC_BOOL_PRED(global, {
    AgencyState const& state = global.state;
    if (state.replicatedLog && state.replicatedLog->plan &&
        state.replicatedLog->plan->currentTerm) {
      auto const& term = *state.replicatedLog->plan->currentTerm;
      if (term.leader) {
        auto const& leader = *term.leader;
        auto const& health = global.state.health;
        return health.validRebootId(leader.serverId, leader.rebootId) &&
               health.notIsFailed(leader.serverId);
      }
    }
    return false;
  });
}

static inline auto serverIsLeader(std::string_view id) {
  return MC_BOOL_PRED(global, {
    AgencyState const& state = global.state;
    if (state.replicatedLog && state.replicatedLog->plan &&
        state.replicatedLog->plan->currentTerm) {
      auto const& term = *state.replicatedLog->plan->currentTerm;
      if (term.leader) {
        auto const& leader = *term.leader;
        return leader.serverId == id;
      }
    }
    return false;
  });
}

}  // namespace arangodb::test::mcpreds
