/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#ifndef FBLUA_THRIFT_SERIALIZATION_H_
#define FBLUA_THRIFT_SERIALIZATION_H_

#include <memory>
#include <unordered_map>

#include <lua.hpp>

#include <folly/Optional.h>
#include <folly/io/IOBuf.h>
#include <fblualib/thrift/if/gen-cpp2/LuaObject_types.h>
#include <thpp/Storage.h>

namespace fblualib { namespace thrift {

// Set the Lua function at the given stack index as the "special serialization
// callback". This is called for all tables that have a metatable in order to
// serialize OO types differently -- for example, to serialize unique
// identifiers (type names) instead of method implementations.
//
// Note that the default behavior works for most purposes; objects in Lua are
// just tables, and their classes (set as metatables) are tables too, so the
// default serialization mechanism would work in most cases. It would, however,
// serialize method implementations as bytecode, which may not be what you want
// (it would be somewhat surprising if, for example, loading a config file
// would revert your object implementation to the one in use when the config
// file was written). Also, we can't serialize methods implemented in C
// (we raise an error in that case).
//
// The API is:
//
// special_key, special_val, table, metatable = callback(table)
//
// special_key: if not nil, key to pass to the deserialization callback
// special_val: value to pass to the deserialization callback
// table:       if nil, serialize current object;
//              otherwise, serialize given table instead
// metatable:   if nil, serialize current metatable;
//              if false, serialize NO metatable;
//              otherwise, serialize given metatable instead
//
// (The distinction between special_key and special_val is arbitrary;
// the Lua component of this library uses special_key to distinguish
// between multiple OOP schemes, and special_val as a type name if
// appropriate)
//
// For example, most OOP implementations use the metatable as the object's
// "class"; the implementation is functions in the metatable, and you don't
// want to serialize their bytecode.
//
// So your callback would, for each object with a matching metatable,
// return (unique_id_for_this_metatable, '', nil, false)
//
// Torch serialization goes one level further and maintains a global map
// of type names -> metatable (so a class is uniquely identified by its
// typename). In that case, your callback could return, for each torch
// objects, ('torch', type_name, nil, false)
void setSpecialSerializationCallback(lua_State* L, int index);

// Set the Lua function at the given stack index as the "special
// deserialization callback". This is called for all serialized tables that
// were serialized with a special callback (that returned a non-nil
// special_key)
//
// The API is:
//
// callback(special_key, special_val, table)
//
// table isinitialized with the deserialized value (and the metatable is set
// appropriately); the callback must mutate table in place.
//
// For most OOP implementations, this only consists of a setmetatable call
// to set the object's class appropriately. The callback would find the
// appropriate metatable based on special_key and special_val.
//
// For the torch example above, the callback could be:
//
// local function deserialize_cb(key, val, obj)
//     if key == 'torch' then
//         setmetatable(obj, torch.getmetatable(val))
//     end
// end
void setSpecialDeserializationCallback(lua_State* L, int index);

struct SerializerOptions {
  constexpr SerializerOptions() { }
  thpp::SharingMode sharing = thpp::SHARE_IOBUF_MANAGED;
  bool localMode = false;
};

// You may register callbacks to serialize custom full userdata types.
//
// The callbacks are registered under a unique string key. The same callbacks
// must be registered under the same key at deserialization time.
//
// In local-only mode (see the comments for the Serializer class), you register
// one callback (a "factory"); the factory creates an object of a class
// derived from MemUserData.
//
// MemUserData must implement
// - doKey(), to return the same key as that supplied at registration time, and
// - doLuaPush(), to reconstruct the original Lua userdata (by pushing it onto
//   a possibly different lua_State in the same process).  We check that the
//   reconstructed object has the same metadata as that given at registration
//   time.
//
// In non-local mode, you register a second callback (a "deserializer"),
// and your MemUserData must also implement doSerialize(). doSerialize()
// and the deserializer callback convert your userdata between the in-memory
// form and an IOBuf.
namespace detail {

class MemUserDataBase {
 public:
  void luaPush(lua_State* L) { doLuaPush(L); }

  virtual ~MemUserDataBase() { }

  LuaRefObject serializeObject(const SerializerOptions& options) const {
    return doSerializeObject(options);
  }

 protected:
  [[noreturn]] void failNYI(const char* msg) const;

 private:
  // Implement this: push a Lua userdata corresponding to this object
  virtual void doLuaPush(lua_State* L);

  // You probably want doSerialize in MemUserData rather than this;
  // doSerializeObject is only used for Torch Tensor and Storage objects
  // (which are handled separately for historical reasons)
  virtual LuaRefObject doSerializeObject(const SerializerOptions& options)
    const;
};

}  // namespace detail

class MemUserData : public detail::MemUserDataBase {
 public:
  folly::IOBuf serialize(const SerializerOptions& options) const {
    return doSerialize(options);
  }
  folly::StringPiece key() const { return doKey(); }

 private:
  LuaRefObject doSerializeObject(const SerializerOptions& options) const final;

  // Implement this: convert from in-memory form to IOBuf
  virtual folly::IOBuf doSerialize(const SerializerOptions& options) const;

  // Implement this: return unique key that matches the one given to
  // registerUserDataCallbacks
  virtual folly::StringPiece doKey() const;
};


// Create a MemUserData from a Lua userdata object at the given index on
// the Lua stack
using MemUserDataFactory =
  std::function<std::unique_ptr<MemUserData>(lua_State*, int)>;

// Create a MemUserData from the given serialized representation
using MemUserDataDeserializer =
  std::function<std::unique_ptr<MemUserData>(const folly::IOBuf&)>;

class Serializer;
class Deserializer;

// In-memory serialized data set; may be converted to a fully-serialized
// (Thrift) version by calling makePortable(). Opaque; pass around to a
// Deserialize object.
class MemSerializedData {
  friend class Serializer;
  friend class Deserializer;
 public:
  MemSerializedData() { }

  LuaRefList& makePortable(
      const SerializerOptions& options = SerializerOptions());
  bool isPortable() const {
    return isPortable_;
  }

 private:
  LuaRefList luaRefs_;
  std::vector<std::unique_ptr<detail::MemUserDataBase>> memRefs_;
  bool isPortable_ = true;
};

// Register serialization / deserialization callbacks for userdata objects
// whose metatable is at mtIndex. "key" must be unique among all custom
// userdata objects.
void registerUserDataCallbacks(
    lua_State* L,
    folly::StringPiece key,
    int mtIndex,
    MemUserDataFactory factory,
    MemUserDataDeserializer deserializer = nullptr);

// If you don't care about the local-mode / fully-serialized distinction,
// you may use a simpler API:

using MemUserDataIOBufSerializer =
  std::function<folly::IOBuf(lua_State* L, int index)>;
using MemUserDataIOBufDeserializer =
  std::function<void(lua_State* L, const folly::IOBuf& buf)>;

void registerUserDataCallbacks(
    lua_State* L,
    folly::StringPiece key,
    int mtIndex,
    MemUserDataIOBufSerializer serializer,
    MemUserDataIOBufDeserializer deserializer);

// Unregister any serialization / deserialization callbacks registered
// under the given key.
void unregisterUserDataCallbacks(lua_State* L, folly::StringPiece key);

// Serialization:
//
// In the common case of serializing only one object, use Serializer::toThrift.
//
// A serializer may serialize multiple objects to Thrift. Objects serialized
// during the same iteration (before a call to finish()) will be de-duplicated
// and serialized only once.
//
// Usage:
// - create the Serializer object
// - set the inverted environment, if desired. This allows serializing
//   unique names instead of specific objects; see below.
// - serialize one or more objects using serialize()
// - call finish() and retrieve the list of deduplicated references and
//   release resources, after which the serializer may be used again.
//
// Both the LuaPrimitiveObjects returned by serialize() and the list of
// LuaRefObjects returned by finish() must be sent to the receiving side.
//
// Inverted environment: some objects that are reachable by traversing
// the dependency graph (as members of tables, or upvalues of functions)
// can't / shouldn't be serialized, but you can assume that they're available
// at the other end. (For example, you shouldn't try to serialize modules
// or C functions.)
//
// In this case, you may memoize these objects into the "inverted environment":
// a map from these objects to their unique names. The unique names are
// tuples of two primitive values (usually numbers or strings).
//
// Passing inverted_env as {[foo] = {1, 'foo'}, [bar] = {2, 'bar'}} will not
// serialize objects foo and bar if encountered, but will replace them
// by their names {1, 'foo'} and {2, 'bar'}. At deserialization time, the same
// environment must be present, and the deserialization code will replace them
// with references to foo and bar on the receiving side.
//
// Normally, inverted_env is created from a list of name -> object tables;
// for example, to avoid serializing all loaded modules, pass package.loaded
// as one of the lists. The helper Lua function invert_env (in
// fb/thrift/init.lua) converts from a table of tables to the inverted_env
// format. (This is why the unique names are tuples; they're pairs of
// outer_table_key (usually a number), inner_table_key (number or string)).
//
// Example:
//
// Assuming that you have modules 'io' and 'ffi' loaded, and you also
// have a bunch of other objects that you don't want to serialize:
//
// local buf = ffi.new('char [?]', 100)
//
// envs = thrift.invert_envs(package.loaded, {buf})
//
// will produce a map of the form
// {
//   [io] = {1, 'io'},  -- key 'io' in first table
//   [ffi] = {1, 'ffi'},
//   [buf] = {2, 1},    -- key 1 in second table
// }
//
// Note that the deserializer takes the non-inverted environment: a table
// of tables. {package.loaded, {buf}} in our example.
//
// Local-only (in-process) mode: some Lua userdata may only be relevant
// in the same process (and can't be serialized to disk), or may have a
// more efficient in-memory representation (efficiency lost if serializing to /
// deserializing from IOBuf). In that case, use finishLocal() rather than
// finish(). The returned object is portable across lua_State in the same
// process, but not across machines (unless you call makePortable on it).
class Serializer {
 public:
  using Options = SerializerOptions;
  explicit Serializer(lua_State* L, Options options=Options());
  ~Serializer();

  static LuaObject toThrift(lua_State* L, int index,
                            int invEnvIdx = 0,
                            Options options=Options());

  void setInvertedEnv(int invEnvIdx);

  LuaPrimitiveObject serialize(int index);
  LuaRefList finish() {
    return std::move(finishLocal().makePortable(options_));
  }

  MemSerializedData finishLocal();

  lua_State* L() const { return L_; }

 private:
  struct SerializationContext {
    int convertedIdx;
    int invEnvIdx;
  };

  void doSerialize(LuaPrimitiveObject& obj, int index,
                   const SerializationContext& ctx, int level,
                   bool allowRefs=true);
  void doSerializeTable(LuaTable& obj, int index,
                        const SerializationContext& ctx, int level);
  void doSerializeFunction(LuaFunction& obj, int index,
                           const SerializationContext& ctx, int level);
  bool doSerializeUserData(LuaRefObject& ref,
                           int index, int mtIndex, int level);
  void doSerializeMemUserData(
      LuaRefObject& ref,
      std::unique_ptr<detail::MemUserDataBase> memRef);

  lua_State* L_;

  MemSerializedData refs_;
  Options options_;
};

struct DeserializerOptions {
  constexpr DeserializerOptions() { }
  // Allow bytecode? or error out if encountered
  bool allowBytecode = true;
  // Memory sharing
  thpp::SharingMode sharing = thpp::SHARE_IOBUF_MANAGED;
};

// In the common case of deserializing only one object,
// use Deserializer::fromThrift.
//
// A deserializer may deserialize multiple objects from Thrift, if they
// were serialized with the same de-duplicated references (between
// consecutive calls to Serializer::finish()).
//
// Usage:
// - create the Deserializer object
// - set the environment (see the comments for the "envs" argument to
//   fb.thrift.to_file)
// - set the list of references using start(); the list of references must
//   remain valid until finish()
// - deserialize objects using deserialize(); deserialize() pushes the object
//   onto the stack.
// - call finish() to release resources, after which the Deserializer object
//   may be used again.
class Deserializer {
 public:
  using Options = DeserializerOptions;

  explicit Deserializer(lua_State* L, Options options = Options());
  ~Deserializer();

  void setEnv(int envIdx);
  void start(const LuaRefList* refs);
  void start(const MemSerializedData* serializedData);
  int deserialize(const LuaPrimitiveObject& obj);
  void finish();

  static int fromThrift(lua_State* L, const LuaObject& obj,
                        int envIdx = 0,
                        Options options = Options());

 private:
  void doDeserializeRefs();
  int doDeserialize(const LuaPrimitiveObject& obj, int convertedIdx, int level,
                    bool allowRefs=true);
  void doDeserializeFunction(const LuaFunction& obj);
  void doSetTable(int index, int convertedIdx, const LuaTable& obj);
  void doSetUpvalues(int index, int convertedIdx, const LuaFunction& obj);

  lua_State* L_;
  const LuaRefList* refs_;
  const std::vector<std::unique_ptr<detail::MemUserDataBase>>* memUserData_;
  Options options_;
};

}}  // namespaces

#endif /* FBLUA_THRIFT_SERIALIZATION_H_ */
