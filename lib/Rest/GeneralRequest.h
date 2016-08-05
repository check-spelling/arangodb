////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
/// @author Achim Brandt
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_REST_GENERAL_REQUEST_H
#define ARANGODB_REST_GENERAL_REQUEST_H 1

#include "Basics/Common.h"
#include "Endpoint/ConnectionInfo.h"

#include <velocypack/Builder.h>
#include <velocypack/Dumper.h>
#include <velocypack/Options.h>
#include <velocypack/velocypack-aliases.h>

namespace arangodb {
namespace velocypack {
class Builder;
struct Options;
}

namespace basics {
class StringBuffer;
}

class RequestContext;

class GeneralRequest {
  GeneralRequest(GeneralRequest const&) = delete;
  GeneralRequest& operator=(GeneralRequest const&) = delete;

 public:
  GeneralRequest(GeneralRequest&&) = default;

  // VSTREAM_CRED: This method is used for sending Authentication
  // request,i.e; username and password.
  //
  // VSTREAM_REGISTER: This Method is used for registering event of
  // some kind
  //
  // VSTREAM_STATUS: Returns STATUS code and message for a given
  // request
  enum class RequestType {
    DELETE_REQ = 0,  // windows redefines DELETE
    GET,
    HEAD,
    OPTIONS,
    POST,
    PUT,
    PATCH,
    VSTREAM_CRED,
    VSTREAM_REGISTER,
    VSTREAM_STATUS,
    ILLEGAL  // must be last
  };

  enum class ProtocolVersion { HTTP_1_0, HTTP_1_1, VPP_1_0, UNKNOWN };
  enum class ContentType {
    CUSTOM,  // use Content-Type from _headers
    JSON,    // application/json
    VPACK,   // application/x-velocypack
    TEXT,    // text/plain
    HTML,    // text/html
    DUMP,    // application/x-arango-dump
    UNSET
  };

 public:
  // translate the HTTP protocol version
  static std::string translateVersion(ProtocolVersion);

  // translate an RequestType enum value into an "HTTP method string"
  static std::string translateMethod(RequestType);

  // translate "HTTP method string" into RequestType enum value
  static RequestType translateMethod(std::string const&);

  // append RequestType as string value to given String buffer
  static void appendMethod(RequestType, arangodb::basics::StringBuffer*);

 protected:
  static RequestType findRequestType(char const*, size_t const);

 public:
  explicit GeneralRequest(ConnectionInfo const& connectionInfo)
      : _version(ProtocolVersion::UNKNOWN),
        _protocol(""),
        _connectionInfo(connectionInfo),
        _clientTaskId(0),
        _requestContext(nullptr),
        _isRequestContextOwner(false),
        _type(RequestType::ILLEGAL),
        _contentType(ContentType::UNSET),
        _contentTypeResponse(ContentType::UNSET) {}

  virtual ~GeneralRequest();

 public:
  ProtocolVersion protocolVersion() const { return _version; }
  char const* protocol() const { return _protocol; }  // http, https or vpp
  void setProtocol(char const* protocol) { _protocol = protocol; }

  ConnectionInfo const& connectionInfo() const { return _connectionInfo; }
  void setConnectionInfo(ConnectionInfo const& connectionInfo) {
    _connectionInfo = connectionInfo;
  }

  uint64_t clientTaskId() const { return _clientTaskId; }
  void setClientTaskId(uint64_t clientTaskId) { _clientTaskId = clientTaskId; }

  std::string const& databaseName() const { return _databaseName; }
  void setDatabaseName(std::string const& databaseName) {
    _databaseName = databaseName;
  }

  // the authenticated user
  std::string const& user() const { return _user; }
  void setUser(std::string const& user) { _user = user; }
  void setUser(std::string&& user) { _user = std::move(user); }

  RequestContext* requestContext() const { return _requestContext; }
  void setRequestContext(RequestContext*, bool);

  RequestType requestType() const { return _type; }
  void setRequestType(RequestType type) { _type = type; }

  std::string const& fullUrl() const { return _fullUrl; }
  void setFullUrl(char const* begin, char const* end);
  void setFullUrl(std::string url);

  // consists of the URL without the host and without any parameters.
  std::string const& requestPath() const { return _requestPath; }
  void setRequestPath(std::string const& requestPath) {
    _requestPath = requestPath;
  }

  // The request path consists of the URL without the host and without any
  // parameters.  The request path is split into two parts: the prefix, namely
  // the part of the request path that was match by a handler and the suffix
  // with all the remaining arguments.
  std::string prefix() const { return _prefix; }
  void setPrefix(std::string const& prefix) { _prefix = prefix; }
  void setPrefix(std::string&& prefix) { _prefix = std::move(prefix); }

  std::vector<std::string> const& suffix() const { return _suffix; }
  void addSuffix(std::string&& part);

  virtual int64_t contentLength() const = 0;
  // get value from headers map. The key must be lowercase.
  virtual std::string const& header(std::string const& key) const = 0;
  virtual std::string const& header(std::string const& key,
                                    bool& found) const = 0;
  // return headers map
  virtual std::unordered_map<std::string, std::string> const& headers()
      const = 0;

  // the value functions give access to to query string parameters
  virtual std::string const& value(std::string const& key) const = 0;
  virtual std::string const& value(std::string const& key,
                                   bool& found) const = 0;
  virtual std::unordered_map<std::string, std::string> values() const = 0;
  virtual std::unordered_map<std::string, std::vector<std::string>>
  arrayValues() const = 0;

  bool velocyPackResponse() const;

  // should toVelocyPack be renamed to payload?
  virtual VPackSlice payload(arangodb::velocypack::Options const*
                                 options = &VPackOptions::Defaults) = 0;

  std::shared_ptr<VPackBuilder> toVelocyPackBuilderPtr(
      arangodb::velocypack::Options const* options) {
    auto rv = std::make_shared<VPackBuilder>();
    rv->add(payload(options));
    return rv;
  };

  ContentType contentType() const { return _contentType; }
  ContentType contentTypeResponse() const { return _contentTypeResponse; }

 protected:
  ProtocolVersion _version;
  char const* _protocol;  // http, https or vpp

  // connection info
  ConnectionInfo _connectionInfo;
  uint64_t _clientTaskId;
  std::string _databaseName;
  std::string _user;

  // request context
  RequestContext* _requestContext;
  bool _isRequestContextOwner;

  // information about the payload
  RequestType _type;  // GET, POST, ..
  std::string _fullUrl;
  std::string _requestPath;
  std::string _prefix;  // part of path matched by rest route
  std::vector<std::string> _suffix;
  ContentType _contentType;  // UNSET, VPACK, JSON
  ContentType _contentTypeResponse;
};
}

#endif
