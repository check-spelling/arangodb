////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2021-2022 ArangoDB GmbH, Cologne, Germany
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
#include <utility>

#include <Futures/Future.h>

#include <Replication2/ReplicatedLog/ILogInterfaces.h>
#include <Replication2/ReplicatedLog/LogCommon.h>
#include <Replication2/ReplicatedLog/types.h>

#include <Replication2/Streams/StreamSpecification.h>
#include <Replication2/Streams/Streams.h>

namespace arangodb::replication2::replicated_log {
struct ILogFollower;
struct ILogLeader;
}  // namespace arangodb::replication2::replicated_log

namespace arangodb::replication2::streams {

/**
 * Common stream dispatcher class for Multiplexer and Demultiplexer. You can
 * obtain a stream given its id using getStreamById. Alternatively, you can
 * static_cast the a pointer to StreamBase<Descriptor> for the given stream.
 * @tparam Self
 * @tparam Spec
 * @tparam StreamType
 */
template<typename Self, typename Spec, template<typename> typename StreamType>
struct LogMultiplexerStreamDispatcher : std::enable_shared_from_this<Self>,
                                        StreamDispatcherBase<Spec, StreamType> {
  template<StreamId Id,
           typename Descriptor = stream_descriptor_by_id_t<Id, Spec>>
  auto getStreamBaseById()
      -> std::shared_ptr<StreamGenericBase<Descriptor, StreamType>> {
    return getStreamByDescriptor<Descriptor>();
  }

  template<StreamId Id>
  auto getStreamById()
      -> std::shared_ptr<StreamType<stream_type_by_id_t<Id, Spec>>> {
    return getStreamByDescriptor<stream_descriptor_by_id_t<Id, Spec>>();
  }

  template<typename Descriptor>
  auto getStreamByDescriptor()
      -> std::shared_ptr<StreamGenericBase<Descriptor, StreamType>> {
    return std::static_pointer_cast<StreamGenericBase<Descriptor, StreamType>>(
        this->shared_from_this());
  }
};

/**
 * Demultiplexer class. Use ::construct to create an instance.
 * @tparam Spec Log specification
 */
template<typename Spec>
struct LogDemultiplexer
    : LogMultiplexerStreamDispatcher<LogDemultiplexer<Spec>, Spec, Stream> {
  virtual auto digestIterator(LogIterator& iter) -> void = 0;

  /*
   * After construction the demultiplexer is not yet in a listen state. You have
   * to call `listen` once.
   */
  virtual auto listen() -> void = 0;

  static auto construct(
      std::shared_ptr<arangodb::replication2::replicated_log::ILogParticipant>)
      -> std::shared_ptr<LogDemultiplexer>;

 protected:
  LogDemultiplexer() = default;
};

/**
 * Multiplexer class. Use ::construct to create an instance.
 * @tparam Spec Log specification
 */
template<typename Spec>
struct LogMultiplexer : LogMultiplexerStreamDispatcher<LogMultiplexer<Spec>,
                                                       Spec, ProducerStream> {
  static auto construct(
      std::shared_ptr<arangodb::replication2::replicated_log::ILogLeader>
          leader) -> std::shared_ptr<LogMultiplexer>;

  /*
   * After construction the multiplexer has an empty internal state. To populate
   * it with the existing state in the replicated log, call
   * `digestAvailableEntries`.
   */
  virtual void digestAvailableEntries() = 0;

 protected:
  LogMultiplexer() = default;
};

}  // namespace arangodb::replication2::streams
