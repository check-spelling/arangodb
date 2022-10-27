////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////
#include "Basics/Common.h"
#include "Basics/DownCast.h"

#include "IResearchLinkHelper.h"

#include <velocypack/Iterator.h>

#include "IResearchCommon.h"
#include "IResearchFeature.h"
#include "IResearchLink.h"
#include "IResearchLinkMeta.h"
#include "IResearchView.h"
#include "IResearchViewCoordinator.h"
#include "VelocyPackHelper.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Cluster/ClusterMethods.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/CollectionNameResolver.h"
#include "Utils/ExecContext.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Indexes.h"

namespace {

#ifdef USE_ENTERPRISE
bool isIgnoredHiddenEnterpriseCollection(std::string const& cName) {
  /*
   * Note: As IResearchView.cpp L204 says:
   * -> "create links on a best-effort basis, link creation failure does not
   * cause view creation failure"
   *
   * Workaround: If we detect a collection which should not be created in the
   * SingleServer case, let the link validation itself return a success.
   *
   * Nevertheless, the user will be notified that there has been an edge case.
   * This should be fine. Another attempt could be to rewrite the links itself,
   * but more code changes will then be necessary.
   */
  if (arangodb::ServerState::instance()->isSingleServer()) {
    if (cName[0] == '_') {
      if (strncmp(cName.c_str(),
                  arangodb::StaticStrings::FullLocalPrefix.c_str(),
                  arangodb::StaticStrings::FullLocalPrefix.size()) == 0 ||
          strncmp(cName.c_str(),
                  arangodb::StaticStrings::FullFromPrefix.c_str(),
                  arangodb::StaticStrings::FullFromPrefix.size()) == 0 ||
          strncmp(cName.c_str(), arangodb::StaticStrings::FullToPrefix.c_str(),
                  arangodb::StaticStrings::FullToPrefix.size()) == 0) {
        LOG_TOPIC("d921b", DEBUG, arangodb::Logger::VIEWS)
            << "Ignoring link to '" << cName
            << "'. Will only be initially created via SmartGraphs of a full "
               "dump of a cluster."
               "This link is not supposed to be restored in case you dump from "
               "a cluster and then restore into a single-server instance.";
        return true;
      }
    }
  }
  return false;
}
#endif

using namespace arangodb;
using namespace arangodb::iresearch;

Result canUseAnalyzers(IResearchLinkMeta const& meta,
                       TRI_vocbase_t const& defaultVocbase) {
  for (auto& pool : meta._analyzerDefinitions) {
    if (!pool) {
      continue;  // skip invalid entries
    }

    auto result = IResearchAnalyzerFeature::canUse(
        IResearchAnalyzerFeature::normalize(pool->name(),
                                            defaultVocbase.name()),
        auth::Level::RO);

    if (!result) {
      return {
          TRI_ERROR_FORBIDDEN,
          std::string("read access is forbidden to arangosearch analyzer '") +
              pool->name() + "'"};
    }
  }

  //  for (auto& field: meta._fields) {
  //    TRI_ASSERT(field.value().get()); // ensured by UniqueHeapInstance
  //    constructor auto& entry = field.value(); auto res =
  //    canUseAnalyzers(*entry, defaultVocbase);
  //
  //    if (!res.ok()) {
  //      return res;
  //    }
  //  }

  return {};
}

Result createLink(LogicalCollection& collection, LogicalView const& view,
                  velocypack::Slice definition) {
  try {
    bool isNew = false;
    auto link = collection.createIndex(definition, isNew);

    if (!(link && isNew)) {
      return {TRI_ERROR_INTERNAL,
              std::string("failed to create link between arangosearch view '") +
                  view.name() + "' and collection '" + collection.name() + "'"};
    }

    // ensure link is synchronized after upgrade in single-server
    if (ServerState::instance()->isSingleServer()) {
      auto& server = collection.vocbase().server();
      auto& db = server.getFeature<DatabaseFeature>();

      if (db.checkVersion() || db.upgrade()) {
        // FIXME find a better way to retrieve an IResearch Link
        // cannot use static_cast/reinterpret_cast since Index is not related to
        // IResearchLink
        auto impl = std::dynamic_pointer_cast<IResearchLink>(link);

        if (impl) {
          return impl->commit();
        }
      }
    }
  } catch (basics::Exception const& e) {
    return Result(e.code(), e.what());
  }

  return Result();
}

Result createLink(LogicalCollection& collection,
                  IResearchViewCoordinator const& view,
                  velocypack::Slice definition) {
  if (ClusterMethods::filterHiddenCollections(collection)) {
    // Enterprise variant, we only need to create links on non-hidden
    // collections (e.g. in SmartGraph Case)
    // The hidden collections are managed by the logic around the
    // SmartEdgeCollection and do not allow to have their own modifications.
    return TRI_ERROR_NO_ERROR;
  }

  std::function<bool(irs::string_ref)> const acceptor =
      [](std::string_view key) -> bool {
    return key != arangodb::StaticStrings::IndexType &&
           key != arangodb::iresearch::StaticStrings::ViewIdField;
  };

  velocypack::Builder builder;
  builder.openObject();
  builder.add(arangodb::StaticStrings::IndexType,
              velocypack::Value(
                  arangodb::iresearch::StaticStrings::ViewArangoSearchType));
  builder.add(arangodb::iresearch::StaticStrings::ViewIdField,
              velocypack::Value(view.guid()));
  if (!mergeSliceSkipKeys(builder, definition, acceptor)) {
    return {TRI_ERROR_INTERNAL,
            std::string("failed to generate definition while creating link "
                        "between arangosearch view '") +
                view.name() + "' and collection '" + collection.name() + "'"};
  }
  builder.close();

  velocypack::Builder tmp;
  return methods::Indexes::ensureIndex(&collection, builder.slice(), true, tmp);
}

template<typename ViewType>
Result dropLink(LogicalCollection& collection, IResearchLink const& link) {
  // don't need to create an extra transaction inside
  // arangodb::methods::Indexes::drop(...)
  if (!collection.dropIndex(link.id())) {
    return {TRI_ERROR_INTERNAL, std::string("failed to drop link '") +
                                    std::to_string(link.id().id()) +
                                    "' from collection '" + collection.name() +
                                    "'"};
  }

  return {};
}

template<>
Result dropLink<IResearchViewCoordinator>(LogicalCollection& collection,
                                          IResearchLink const& link) {
  if (ClusterMethods::filterHiddenCollections(collection)) {
    // Enterprise variant, we only need to drop links on non-hidden
    // collections (e.g. in SmartGraph Case)
    // The hidden collections are managed by the logic around the
    // SmartEdgeCollection and do not allow to have their own modifications.
    return {};
  }

  velocypack::Builder builder;
  builder.openObject();
  builder.add(arangodb::StaticStrings::IndexId,
              velocypack::Value(link.id().id()));
  builder.close();

  return methods::Indexes::drop(&collection, builder.slice());
}

struct State {
  std::shared_ptr<LogicalCollection> _collection;
  // std::numeric_limits<size_t>::max() == removal only
  size_t _collectionsToLockOffset;
  std::shared_ptr<IResearchLink> _link;
  size_t _linkDefinitionsOffset;
  Result _result;       // operation result
  bool _stale = false;  // request came from the stale list
  explicit State(size_t collectionsToLockOffset)
      : State(collectionsToLockOffset, std::numeric_limits<size_t>::max()) {}
  State(size_t collectionsToLockOffset, size_t linkDefinitionsOffset)
      : _collectionsToLockOffset(collectionsToLockOffset),
        _linkDefinitionsOffset(linkDefinitionsOffset) {}
};

template<typename ViewType>
Result modifyLinks(std::unordered_set<DataSourceId>& modified, ViewType& view,
                   velocypack::Slice links, LinkVersion defaultVersion,
                   std::unordered_set<DataSourceId> const& stale) {
  LOG_TOPIC("4bdd2", DEBUG, arangodb::iresearch::TOPIC)
      << "link modification request for view '" << view.name()
      << "', original definition:" << links.toString();

  if (!links.isObject()) {
    return {
        TRI_ERROR_BAD_PARAMETER,
        std::string(
            "error parsing link parameters from json for arangosearch view '") +
            view.name() + "'"};
  }

  std::vector<std::string> collectionsToLock;
  std::vector<std::pair<velocypack::Builder, IResearchLinkMeta>>
      linkDefinitions;
  std::vector<State> linkModifications;

  for (velocypack::ObjectIterator linksItr(links); linksItr.valid();
       ++linksItr) {
    auto collection = linksItr.key();

    if (!collection.isString()) {
      return {TRI_ERROR_BAD_PARAMETER,
              std::string("error parsing link parameters from json for "
                          "arangosearch view '") +
                  view.name() + "' offset '" +
                  basics::StringUtils::itoa(linksItr.index()) + '"'};
    }

    auto link = linksItr.value();
    auto collectionName = collection.copyString();

    if (link.isNull()) {
      linkModifications.emplace_back(collectionsToLock.size());
      collectionsToLock.emplace_back(collectionName);

      continue;  // only removal requested
    }

    velocypack::Builder normalized;

    normalized.openObject();

    // DBServerAgencySync::getLocalCollections(...) generates
    // 'forPersistence' definitions that are then compared in
    // Maintenance.cpp:compareIndexes(...) via
    // arangodb::Index::Compare(...)
    // hence must use 'isCreation=true' for normalize(...) to match
    // normalize to validate analyzer definitions
    auto res = IResearchLinkHelper::normalize(
        normalized, link, true, view.vocbase(), defaultVersion,
        &view.primarySort(), &view.primarySortCompression(),
        &view.storedValues(), link.get(arangodb::StaticStrings::IndexId),
        collectionName);

    if (!res.ok()) {
      return res;
    }

    normalized.close();
    link = normalized.slice();  // use normalized definition for index creation

    LOG_TOPIC("4bdd1", DEBUG, arangodb::iresearch::TOPIC)
        << "link modification request for view '" << view.name()
        << "', normalized definition:" << link.toString();

    std::function<bool(irs::string_ref)> const acceptor =
        [](std::string_view key) -> bool {
      return key != arangodb::StaticStrings::IndexType &&
             key != arangodb::iresearch::StaticStrings::ViewIdField;
    };

    velocypack::Builder namedJson;
    namedJson.openObject();
    namedJson.add(
        arangodb::StaticStrings::IndexType,
        velocypack::Value(
            arangodb::iresearch::StaticStrings::ViewArangoSearchType));
    namedJson.add(arangodb::iresearch::StaticStrings::ViewIdField,
                  velocypack::Value(view.guid()));
    if (!mergeSliceSkipKeys(namedJson, link, acceptor)) {
      return {TRI_ERROR_INTERNAL,
              std::string("failed to update link definition with the view name "
                          "while updating arangosearch view '") +
                  view.name() + "' collection '" + collectionName + "'"};
    }
    namedJson.close();

    std::string error;
    IResearchLinkMeta linkMeta;

    // validated and normalized with 'isCreation=true' above via normalize(...)
    if (!linkMeta.init(view.vocbase().server(), namedJson.slice(), error,
                       view.vocbase().name())) {
      return {TRI_ERROR_BAD_PARAMETER,
              std::string("error parsing link parameters from json for "
                          "arangosearch view '") +
                  view.name() + "' collection '" + collectionName +
                  "' error '" + error + "'"};
    }

    linkModifications.emplace_back(collectionsToLock.size(),
                                   linkDefinitions.size());
    collectionsToLock.emplace_back(collectionName);
    linkDefinitions.emplace_back(std::move(namedJson), std::move(linkMeta));
  }

  auto trxCtx = transaction::StandaloneContext::Create(view.vocbase());

  // add removals for any 'stale' links not found in the 'links' definition
  for (auto& id : stale) {
    if (!trxCtx->resolver().getCollection(id)) {
      LOG_TOPIC("4bdd7", WARN, arangodb::iresearch::TOPIC)
          << "request for removal of a stale link to a missing collection '"
          << id << "', ignoring";

      continue;  // skip adding removal requests to stale links to nonexistent
                 // collections (already dropped)
    }

    linkModifications.emplace_back(collectionsToLock.size());
    linkModifications.back()._stale = true;
    collectionsToLock.emplace_back(std::to_string(id.id()));
  }

  if (collectionsToLock.empty()) {
    return {};  // nothing to update
  }

  // required to remove links from non-RW collections
  ExecContextSuperuserScope scope;

  {
    // track removal for potential reindex
    std::unordered_set<DataSourceId> collectionsToRemove;
    // track reindex requests
    std::unordered_set<DataSourceId> collectionsToUpdate;

    // resolve corresponding collection and link
    for (auto itr = linkModifications.begin();
         itr != linkModifications.end();) {
      auto& state = *itr;
      auto& collectionName = collectionsToLock[state._collectionsToLockOffset];

      state._collection = trxCtx->resolver().getCollection(collectionName);

      if (!state._collection) {
        // remove modification state if removal of non-existant link on
        // non-existant collection
        if (state._linkDefinitionsOffset >= linkDefinitions.size()) {
          // link removal request
          itr = linkModifications.erase(itr);

          continue;
        }

        return {
            TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
            std::string(
                "failed to get collection while updating arangosearch view '") +
                view.name() + "' collection '" + collectionName + "'"};
      }

      state._link = IResearchLinkHelper::find(*(state._collection), view);

      // remove modification state if removal of non-existant link
      if (!state._link  // links currently does not exist
          && state._linkDefinitionsOffset >=
                 linkDefinitions.size()) {  // link removal request
        LOG_TOPIC("c7111", TRACE, arangodb::iresearch::TOPIC)
            << "found link for collection '" << state._collection->name()
            << "' - slated for removal";

        view.unlink(
            state._collection
                ->id());  // drop any stale data for the specified collection
        itr = linkModifications.erase(itr);

        continue;
      }

      if (state._link       // links currently exists
          && !state._stale  // did not originate from the stale list (remove
                            // stale links lower)
          && state._linkDefinitionsOffset >=
                 linkDefinitions.size()) {  // link removal request
        LOG_TOPIC("a58da", TRACE, arangodb::iresearch::TOPIC)
            << "found link '" << state._link->id() << "' for collection '"
            << state._collection->name() << "' - slated for removal";
        auto cid = state._collection->id();

        // remove duplicate removal requests (e.g. by name + by CID)
        if (collectionsToRemove.find(cid) !=
            collectionsToRemove.end()) {  // removal previously requested
          itr = linkModifications.erase(itr);

          continue;
        }

        collectionsToRemove.emplace(cid);
      }

      if (state._link  // links currently exists
          && state._linkDefinitionsOffset <
                 linkDefinitions.size()) {  // link update request
        LOG_TOPIC("8419d", TRACE, arangodb::iresearch::TOPIC)
            << "found link '" << state._link->id() << "' for collection '"
            << state._collection->name() << "' - slated for update";
        collectionsToUpdate.emplace(state._collection->id());
      }

      LOG_TOPIC_IF("e9a8c", TRACE, arangodb::iresearch::TOPIC, state._link)
          << "found link '" << state._link->id() << "' for collection '"
          << state._collection->name() << "' - unsure what to do";

      LOG_TOPIC_IF("b01be", TRACE, arangodb::iresearch::TOPIC, !state._link)
          << "no link found for collection '" << state._collection->name()
          << "'";

      ++itr;
    }

    // remove modification state if it came from the stale list and a separate
    // removal or reindex also present
    for (auto itr = linkModifications.begin();
         itr != linkModifications.end();) {
      auto& state = *itr;
      auto cid = state._collection->id();

      // remove modification if came from the stale list and a separate reindex
      // or removal request also present otherwise consider 'stale list
      // requests' as valid removal requests

      // originated from the stale list
      if (state._stale &&
          // also has a removal request (duplicate removal request)
          (collectionsToRemove.find(cid) != collectionsToRemove.end()
           // also has a reindex request
           || collectionsToUpdate.find(cid) != collectionsToUpdate.end())) {
        LOG_TOPIC("5c99e", TRACE, arangodb::iresearch::TOPIC)
            << "modification unnecessary, came from stale list, for link '"
            << state._link->id() << "'";
        itr = linkModifications.erase(itr);
        continue;
      }

      ++itr;
    }

    // remove modification state if no change on existing link and reindex not
    // requested
    for (auto itr = linkModifications.begin();
         itr != linkModifications.end();) {
      auto& state = *itr;

      // remove modification if removal request with an update request also
      // present

      // links currently exists
      if (state._link
          // link removal request
          && state._linkDefinitionsOffset >= linkDefinitions.size()
          // also has a reindex request
          && collectionsToUpdate.find(state._collection->id()) !=
                 collectionsToUpdate.end()) {
        LOG_TOPIC("1d095", TRACE, arangodb::iresearch::TOPIC)
            << "modification unnecessary, remove+update, for link '"
            << state._link->id() << "'";
        itr = linkModifications.erase(itr);
        continue;
      }

      // remove modification state if no change on existing link or

      // links currently exists
      if (state._link
          // link creation request
          && state._linkDefinitionsOffset < linkDefinitions.size()
          // not a reindex request
          && collectionsToRemove.find(state._collection->id()) ==
                 collectionsToRemove.end() &&
          // link meta not modified
          *(state._link) ==
              linkDefinitions[state._linkDefinitionsOffset].second) {
        LOG_TOPIC("4c196", TRACE, arangodb::iresearch::TOPIC)
            << "modification unnecessary, no change, for link '"
            << state._link->id() << "'";
        itr = linkModifications.erase(itr);
        continue;
      }

      ++itr;
    }
  }

  // execute removals
  for (auto& state : linkModifications) {
    if (state._link) {  // link removal or recreate request
      state._result = dropLink<ViewType>(*(state._collection), *(state._link));
      modified.emplace(state._collection->id());
    }
  }

  // execute additions
  for (auto& state : linkModifications) {
    if (state._result.ok()  // valid state (unmodified or after removal)
        && state._linkDefinitionsOffset < linkDefinitions.size()) {
      state._result = createLink(
          *(state._collection), view,
          linkDefinitions[state._linkDefinitionsOffset].first.slice());
      modified.emplace(state._collection->id());
    }
  }

  std::string error;

  // validate success
  for (auto& state : linkModifications) {
    if (!state._result.ok()) {
      error.append(error.empty() ? "" : ", ")
          .append(collectionsToLock[state._collectionsToLockOffset])
          .append(": ")
          .append(std::to_string(static_cast<int>(state._result.errorNumber())))
          .append(" ")
          .append(state._result.errorMessage());
    }
  }

  if (error.empty()) {
    return {};
  }

  return {
      TRI_ERROR_ARANGO_ILLEGAL_STATE,
      "failed to update links while updating arangosearch view '" +
          view.name() +
          "', retry same request or examine errors for collections: " + error};
}

}  // namespace

namespace arangodb {
namespace iresearch {

/*static*/ VPackBuilder IResearchLinkHelper::emptyIndexSlice(
    uint64_t objectId) {
  VPackBuilder builder;
  VPackBuilder fieldsBuilder;

  fieldsBuilder.openArray();
  fieldsBuilder.close();
  builder.openObject();
  if (objectId) {
    builder.add(arangodb::StaticStrings::ObjectId,
                VPackValue(std::to_string(objectId)));
  }
  builder.add(arangodb::StaticStrings::IndexFields, fieldsBuilder.slice());
  builder.add(arangodb::StaticStrings::IndexType,
              velocypack::Value(
                  arangodb::iresearch::StaticStrings::ViewArangoSearchType));
  builder.close();
  return builder;
}

/*static*/ bool IResearchLinkHelper::equal(ArangodServer& server,
                                           velocypack::Slice lhs,
                                           velocypack::Slice rhs,
                                           irs::string_ref dbname) {
  if (!lhs.isObject() || !rhs.isObject()) {
    return false;
  }

  auto lhsViewSlice = lhs.get(StaticStrings::ViewIdField);
  auto rhsViewSlice = rhs.get(StaticStrings::ViewIdField);

  if (!lhsViewSlice.binaryEquals(rhsViewSlice)) {
    if (!lhsViewSlice.isString() || !rhsViewSlice.isString()) {
      return false;
    }

    auto ls = lhsViewSlice.copyString();
    auto rs = rhsViewSlice.copyString();

    if (ls.size() > rs.size()) {
      std::swap(ls, rs);
    }

    // in the cluster, we may have identifiers of the form
    // `cxxx/` and `cxxx/yyy` which should be considered equal if the
    // one is a prefix of the other up to the `/`
    if (ls.empty() || ls.back() != '/' || ls.compare(rs.substr(0, ls.size()))) {
      return false;
    }
  }

  std::string errorField;
  IResearchLinkMeta lhsMeta;
  IResearchLinkMeta rhsMeta;

  return lhsMeta.init(server, lhs, errorField, dbname) &&
         rhsMeta.init(server, rhs, errorField, dbname) && lhsMeta == rhsMeta;
}

/*static*/ std::shared_ptr<IResearchLink> IResearchLinkHelper::find(
    LogicalCollection const& collection, IndexId id) {
  auto index = collection.lookupIndex(id);

  if (!index || Index::TRI_IDX_TYPE_IRESEARCH_LINK != index->type()) {
    return nullptr;  // not an IResearchLink
  }

  // TODO FIXME find a better way to retrieve an IResearch Link
  // cannot use static_cast/reinterpret_cast since Index is not related to
  // IResearchLink
  auto link = std::dynamic_pointer_cast<IResearchLink>(index);

  if (link && link->id() == id) {
    return link;  // found required link
  }

  return nullptr;
}

/*static*/ std::shared_ptr<IResearchLink> IResearchLinkHelper::find(
    LogicalCollection const& collection, LogicalView const& view) {
  for (auto& index : collection.getIndexes()) {
    if (!index || Index::TRI_IDX_TYPE_IRESEARCH_LINK != index->type()) {
      continue;  // not an IResearchLink
    }

    // TODO FIXME find a better way to retrieve an iResearch Link
    // cannot use static_cast/reinterpret_cast since Index is not related to
    // IResearchLink
    auto link = std::dynamic_pointer_cast<IResearchLink>(index);

    if (link && *link == view) {
      return link;  // found required link
    }
  }

  return nullptr;
}

/*static*/ Result IResearchLinkHelper::normalize(
    velocypack::Builder& normalized, velocypack::Slice definition,
    bool isCreation, TRI_vocbase_t const& vocbase, LinkVersion defaultVersion,
    IResearchViewSort const* primarySort, /* = nullptr */
    irs::type_info::type_id const* primarySortCompression /*= nullptr*/,
    IResearchViewStoredValues const* storedValues, /* = nullptr */
    velocypack::Slice idSlice,                     /* = velocypack::Slice()*/
    irs::string_ref collectionName /*= irs::string_ref::NIL*/) {
  if (!normalized.isOpenObject()) {
    return {TRI_ERROR_BAD_PARAMETER,
            "invalid output buffer provided for arangosearch link normalized "
            "definition generation"};
  }

  std::string error;
  IResearchLinkMeta meta;

  // implicit analyzer validation via IResearchLinkMeta done in 2 places:
  // IResearchLinkHelper::normalize(...) if creating via collection API
  // ::modifyLinks(...) (via call to normalize(...) prior to getting
  // superuser) if creating via IResearchLinkHelper API
  if (!meta.init(vocbase.server(), definition, error, vocbase.name(),
                 defaultVersion)) {
    return {TRI_ERROR_BAD_PARAMETER,
            "error parsing arangosearch link parameters from json: " + error};
  }

  // same validation as in modifyLinks(...) for Views API
  auto res = canUseAnalyzers(meta, vocbase);

  if (!res.ok()) {
    return res;
  }

  normalized.add(arangodb::StaticStrings::IndexType,
                 velocypack::Value(
                     arangodb::iresearch::StaticStrings::ViewArangoSearchType));

  if (ServerState::instance()->isClusterRole() && isCreation &&
      !collectionName.empty() && meta._collectionName.empty()) {
    meta._collectionName = collectionName;
#ifdef USE_ENTERPRISE
    ClusterMethods::realNameFromSmartName(meta._collectionName);
#endif
  }

  // copy over IResearch Link identifier
  if (!idSlice.isNone()) {
    if (idSlice.isNumber()) {
      normalized.add(
          arangodb::StaticStrings::IndexId,
          VPackValue(std::to_string(idSlice.getNumericValue<uint64_t>())));
    } else {
      normalized.add(arangodb::StaticStrings::IndexId, idSlice);
    }
  }

  // copy over IResearch View identifier
  if (definition.hasKey(StaticStrings::ViewIdField)) {
    normalized.add(StaticStrings::ViewIdField,
                   definition.get(StaticStrings::ViewIdField));
  }

  if (definition.hasKey(arangodb::StaticStrings::IndexInBackground)) {
    IndexFactory::processIndexInBackground(definition, normalized);
  }

  IndexFactory::processIndexParallelism(definition, normalized);

  if (primarySort) {
    // normalize sort if specified
    meta._sort = *primarySort;
  }

  if (primarySortCompression) {
    meta._sortCompression = *primarySortCompression;
  }

  if (storedValues) {
    // normalize stored values if specified
    meta._storedValues = *storedValues;
  }

  // 'isCreation' is set when forPersistence
  if (!meta.json(vocbase.server(), normalized, isCreation, nullptr, &vocbase)) {
    return {TRI_ERROR_BAD_PARAMETER,
            "error generating arangosearch link normalized definition"};
  }

  return {};
}

/*static*/ Result IResearchLinkHelper::validateLinks(TRI_vocbase_t& vocbase,
                                                     velocypack::Slice links) {
  if (!links.isObject()) {
    return {TRI_ERROR_BAD_PARAMETER,
            std::string("while validating arangosearch link definition, error: "
                        "definition is not an object")};
  }

  size_t offset = 0;
  CollectionNameResolver resolver(vocbase);

  for (velocypack::ObjectIterator itr(links); itr.valid(); ++itr, ++offset) {
    auto collectionName = itr.key();
    auto linkDefinition = itr.value();

    if (!collectionName.isString()) {
      return {TRI_ERROR_BAD_PARAMETER,
              std::string("while validating arangosearch link definition, "
                          "error: collection at offset ") +
                  std::to_string(offset) + " is not a string"};
    }

#if USE_ENTERPRISE
    bool isIgnoredCollection =
        isIgnoredHiddenEnterpriseCollection(collectionName.copyString());
#else
    bool isIgnoredCollection = false;
#endif

    auto collection = resolver.getCollection(collectionName.copyString());

    if (!isIgnoredCollection && !collection) {
      return {TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
              std::string("while validating arangosearch link definition, "
                          "error: collection '") +
                  collectionName.copyString() + "' not found"};
    }

    // check link auth as per https://github.com/arangodb/backlog/issues/459
    if (!ExecContext::current().canUseCollection(
            vocbase.name(), collection->name(), auth::Level::RO)) {
      return {TRI_ERROR_FORBIDDEN,  // code
              std::string("while validating arangosearch link definition, "
                          "error: collection '") +
                  collectionName.copyString() +
                  "' not authorized for read access"};
    }

    IResearchLinkMeta meta;
    std::string errorField;

    if (!linkDefinition.isNull()) {  // have link definition
      // for db-server analyzer validation should have already applied on
      // coordinator
      if (!meta.init(vocbase.server(), linkDefinition, errorField,
                     vocbase.name())) {
        return {TRI_ERROR_BAD_PARAMETER,
                errorField.empty()
                    ? (std::string(
                           "while validating arangosearch link definition, "
                           "error: invalid link definition for collection '") +
                       collectionName.copyString() +
                       "': " + linkDefinition.toString())
                    : (std::string(
                           "while validating arangosearch link definition, "
                           "error: invalid link definition for collection '") +
                       collectionName.copyString() +
                       "' error in attribute: " + errorField)};
      }
      // validate analyzers origin
      // analyzer should be either from same database as view (and collection)
      // or from system database
      {
        const auto& currentVocbase = vocbase.name();
        for (const auto& analyzer : meta._analyzerDefinitions) {
          TRI_ASSERT(analyzer);  // should be checked in meta init
          if (ADB_UNLIKELY(!analyzer)) {
            continue;
          }
          auto* pool = analyzer.get();
          auto analyzerVocbase =
              IResearchAnalyzerFeature::extractVocbaseName(pool->name());

          if (!IResearchAnalyzerFeature::analyzerReachableFromDb(
                  analyzerVocbase, currentVocbase, true)) {
            return {TRI_ERROR_BAD_PARAMETER,
                    std::string("Analyzer '")
                        .append(pool->name())
                        .append("' is not accessible from database '")
                        .append(currentVocbase)
                        .append("'")};
          }
        }
      }
    }
  }

  return {};
}

/*static*/ bool IResearchLinkHelper::visit(
    LogicalCollection const& collection,
    std::function<bool(IResearchLink& link)> const& visitor) {
  for (auto& index : collection.getIndexes()) {
    if (!index || Index::TRI_IDX_TYPE_IRESEARCH_LINK != index->type()) {
      continue;  // not an IResearchLink
    }

    // TODO FIXME find a better way to retrieve an iResearch Link
    // cannot use static_cast/reinterpret_cast since Index is not related to
    // IResearchLink
    auto link = std::dynamic_pointer_cast<IResearchLink>(index);

    if (link && !visitor(*link)) {
      return false;  // abort requested
    }
  }

  return true;
}

/*static*/ Result IResearchLinkHelper::updateLinks(
    std::unordered_set<DataSourceId>& modified, LogicalView& view,
    velocypack::Slice links, LinkVersion defaultVersion,
    std::unordered_set<DataSourceId> const& stale /*= {}*/) {
  LOG_TOPIC("00bf9", TRACE, arangodb::iresearch::TOPIC)
      << "beginning IResearchLinkHelper::updateLinks";
  try {
    if (ServerState::instance()->isCoordinator()) {
      return modifyLinks<IResearchViewCoordinator>(
          modified, basics::downCast<IResearchViewCoordinator>(view), links,
          defaultVersion, stale);
    }

    return modifyLinks<IResearchView>(modified,
                                      basics::downCast<IResearchView>(view),
                                      links, defaultVersion, stale);
  } catch (basics::Exception& e) {
    LOG_TOPIC("72dde", WARN, arangodb::iresearch::TOPIC)
        << "caught exception while updating links for arangosearch view '"
        << view.name() << "': " << e.code() << " " << e.what();

    return {e.code(),
            std::string("error updating links for arangosearch view '") +
                view.name() + "'"};
  } catch (std::exception const& e) {
    LOG_TOPIC("9d5f8", WARN, arangodb::iresearch::TOPIC)
        << "caught exception while updating links for arangosearch view '"
        << view.name() << "': " << e.what();

    return {TRI_ERROR_BAD_PARAMETER,
            std::string("error updating links for arangosearch view '") +
                view.name() + "'"};
  } catch (...) {
    LOG_TOPIC("ff0b6", WARN, arangodb::iresearch::TOPIC)
        << "caught exception while updating links for arangosearch view '"
        << view.name() << "'";

    return {TRI_ERROR_BAD_PARAMETER,
            std::string("error updating links for arangosearch view '") +
                view.name() + "'"};
  }
}

}  // namespace iresearch
}  // namespace arangodb
