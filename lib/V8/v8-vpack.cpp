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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "v8-vpack.h"
#include "Basics/Exceptions.h"
#include "V8/v8-utils.h"

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

////////////////////////////////////////////////////////////////////////////////
/// @brief converts a VelocyValueType::String into a V8 object
////////////////////////////////////////////////////////////////////////////////

static inline v8::Handle<v8::Value> ObjectVPackString(v8::Isolate* isolate,
                                                      VPackSlice const& slice) {
  arangodb::velocypack::ValueLength l;
  char const* val = slice.getString(l);
  return TRI_V8_PAIR_STRING(val, l);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief converts a VelocyValueType::Object into a V8 object
////////////////////////////////////////////////////////////////////////////////

static v8::Handle<v8::Value> ObjectVPackObject(v8::Isolate* isolate,
                                               VPackSlice const& slice,
                                               VPackOptions const* options,
                                               VPackSlice const* base) {
  TRI_ASSERT(slice.isObject());
  v8::Handle<v8::Object> object = v8::Object::New(isolate);

  if (object.IsEmpty()) {
    return v8::Undefined(isolate);
  }

  VPackObjectIterator it(slice);
  while (it.valid()) {
    v8::Handle<v8::Value> val =
        TRI_VPackToV8(isolate, it.value(), options, &slice);
    if (!val.IsEmpty()) {
      auto k = ObjectVPackString(isolate, it.key());
      object->ForceSet(k, val);
    }
    it.next();
  }

  return object;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief converts a VelocyValueType::Array into a V8 object
////////////////////////////////////////////////////////////////////////////////

static v8::Handle<v8::Value> ObjectVPackArray(v8::Isolate* isolate,
                                              VPackSlice const& slice,
                                              VPackOptions const* options,
                                              VPackSlice const* base) {
  TRI_ASSERT(slice.isArray());
  v8::Handle<v8::Array> object =
      v8::Array::New(isolate, static_cast<int>(slice.length()));

  if (object.IsEmpty()) {
    return v8::Undefined(isolate);
  }

  uint32_t j = 0;
  VPackArrayIterator it(slice);

  while (it.valid()) {
    v8::Handle<v8::Value> val =
        TRI_VPackToV8(isolate, it.value(), options, &slice);
    if (!val.IsEmpty()) {
      object->Set(j++, val);
    }
    it.next();
  }

  return object;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief converts a VPack value into a V8 object
////////////////////////////////////////////////////////////////////////////////

v8::Handle<v8::Value> TRI_VPackToV8(v8::Isolate* isolate,
                                    VPackSlice const& slice,
                                    VPackOptions const* options,
                                    VPackSlice const* base) {
  switch (slice.type()) {
    case VPackValueType::Null:
      return v8::Null(isolate);
    case VPackValueType::Bool:
      return v8::Boolean::New(isolate, slice.getBool());
    case VPackValueType::Double: {
      // convert NaN, +inf & -inf to null
      double value = slice.getDouble();
      if (std::isnan(value) || !std::isfinite(value) || value == HUGE_VAL || value == -HUGE_VAL) {
        return v8::Null(isolate);
      }
      return v8::Number::New(isolate, slice.getDouble());
    }
    case VPackValueType::Int: {
      int64_t value = slice.getInt();
      if (value >= -2147483648 && value <= 2147483647) {
        // value is within bounds of an int32_t
        return v8::Integer::New(isolate, static_cast<int32_t>(value));
      }
      if (value >= 0 && value <= 4294967295) {
        // value is within bounds of a uint32_t
        return v8::Integer::NewFromUnsigned(isolate, static_cast<uint32_t>(value));
      }
      // must use double to avoid truncation
      return v8::Number::New(isolate, static_cast<double>(slice.getInt()));
    }
    case VPackValueType::UInt: {
      uint64_t value = slice.getUInt();
      if (value <= 4294967295) {
        // value is within bounds of a uint32_t
        return v8::Integer::NewFromUnsigned(isolate, static_cast<uint32_t>(value));
      }
      // must use double to avoid truncation
      return v8::Number::New(isolate, static_cast<double>(slice.getUInt()));
    }
    case VPackValueType::SmallInt: {
      return v8::Integer::New(isolate, slice.getNumericValue<int32_t>());
    }
    case VPackValueType::String:
      return ObjectVPackString(isolate, slice);
    case VPackValueType::Object:
      return ObjectVPackObject(isolate, slice, options, base);
    case VPackValueType::Array:
      return ObjectVPackArray(isolate, slice, options, base);
    case VPackValueType::Custom: {
      if (options == nullptr || options->customTypeHandler == nullptr || base == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                       "Could not extract custom attribute.");
      }
      std::string id =
          options->customTypeHandler->toString(slice, options, *base);
      return TRI_V8_STD_STRING(id);
    }
    case VPackValueType::None:
    default:
      return v8::Undefined(isolate);
  }
}

struct BuilderContext {
  BuilderContext(v8::Isolate* isolate, VPackBuilder& builder,
                 bool keepTopLevelOpen)
      : isolate(isolate),
        builder(builder),
        keepTopLevelOpen(keepTopLevelOpen) {}

  v8::Isolate* isolate;
  v8::Handle<v8::Value> toJsonKey;
  VPackBuilder& builder;
  std::unordered_set<int> seenHashes;
  std::vector<v8::Handle<v8::Object>> seenObjects;

  bool keepTopLevelOpen;
};

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a VPackValue to either an array or an object
////////////////////////////////////////////////////////////////////////////////

static inline void AddValue(BuilderContext& context, std::string const& attributeName,
                            bool inObject, VPackValue const& value) {
  if (inObject) {
    context.builder.add(attributeName, value);
  } else {
    context.builder.add(value);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief adds a VPackValue to either an array or an object
////////////////////////////////////////////////////////////////////////////////

static inline void AddValuePair(BuilderContext& context, std::string const& attributeName,
                                bool inObject, VPackValuePair const& value) {
  if (inObject) {
    context.builder.add(attributeName, value);
  } else {
    context.builder.add(value);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief convert a V8 value to a VPack value
////////////////////////////////////////////////////////////////////////////////

template <bool performAllChecks>
static int V8ToVPack(BuilderContext& context,
                     v8::Handle<v8::Value> const parameter,
                     std::string const& attributeName, bool inObject) {

  if (parameter->IsNull() || parameter->IsUndefined()) {
    AddValue(context, attributeName, inObject,
             VPackValue(VPackValueType::Null));
    return TRI_ERROR_NO_ERROR;
  }

  if (parameter->IsBoolean()) {
    v8::Handle<v8::Boolean> booleanParameter = parameter->ToBoolean();
    AddValue(context, attributeName, inObject,
             VPackValue(booleanParameter->Value()));
    return TRI_ERROR_NO_ERROR;
  }
  
  if (parameter->IsInt32()) {
    v8::Handle<v8::Int32> numberParameter = parameter->ToInt32();
    AddValue(context, attributeName, inObject,
             VPackValue(numberParameter->Value()));
    return TRI_ERROR_NO_ERROR;
  }
  
  if (parameter->IsUint32()) {
    v8::Handle<v8::Uint32> numberParameter = parameter->ToUint32();
    AddValue(context, attributeName, inObject,
             VPackValue(numberParameter->Value()));
    return TRI_ERROR_NO_ERROR;
  }

  if (parameter->IsNumber()) {
    v8::Handle<v8::Number> numberParameter = parameter->ToNumber();
    AddValue(context, attributeName, inObject,
             VPackValue(numberParameter->Value()));
    return TRI_ERROR_NO_ERROR;
  }

  if (parameter->IsString()) {
    v8::Handle<v8::String> stringParameter = parameter->ToString();
    v8::String::Utf8Value str(stringParameter);

    if (*str == nullptr) {
      return TRI_ERROR_OUT_OF_MEMORY;
    }

    AddValuePair(context, attributeName, inObject, VPackValuePair(*str, str.length(), VPackValueType::String));
    return TRI_ERROR_NO_ERROR;
  }

  if (parameter->IsArray()) {
    v8::Handle<v8::Array> array = v8::Handle<v8::Array>::Cast(parameter);

    AddValue(context, attributeName, inObject,
             VPackValue(VPackValueType::Array));
    uint32_t const n = array->Length();

    for (uint32_t i = 0; i < n; ++i) {
      v8::Handle<v8::Value> value = array->Get(i);
      if (value->IsUndefined()) {
        // ignore object values which are undefined
        continue;
      }
      int res = V8ToVPack<performAllChecks>(context, value, "", false);

      if (res != TRI_ERROR_NO_ERROR) {
        return res;
      }
    }

    if (performAllChecks) {
      if (!context.keepTopLevelOpen || !context.seenObjects.empty()) {
        context.builder.close();
      }
    } else {
      if (!context.keepTopLevelOpen) {
        context.builder.close();
      }
    }
    return TRI_ERROR_NO_ERROR;
  }

  if (parameter->IsObject()) {
    if (parameter->IsBooleanObject()) {
      AddValue(context, attributeName, inObject,
               VPackValue(v8::Handle<v8::BooleanObject>::Cast(parameter)
                              ->BooleanValue()));
      return TRI_ERROR_NO_ERROR;
    }

    if (parameter->IsNumberObject()) {
      AddValue(context, attributeName, inObject,
               VPackValue(v8::Handle<v8::NumberObject>::Cast(parameter)
                              ->NumberValue()));
      return TRI_ERROR_NO_ERROR;
    }

    if (parameter->IsStringObject()) {
      v8::Handle<v8::String> stringParameter(parameter->ToString());
      v8::String::Utf8Value str(stringParameter);

      if (*str == nullptr) {
        return TRI_ERROR_OUT_OF_MEMORY;
      }

      AddValuePair(context, attributeName, inObject, VPackValuePair(*str, str.length(), VPackValueType::String));
      return TRI_ERROR_NO_ERROR;
    }

    if (performAllChecks) {
      if (parameter->IsRegExp() || parameter->IsFunction() ||
          parameter->IsExternal()) {
        return TRI_ERROR_BAD_PARAMETER;
      }
    }

    v8::Handle<v8::Object> o = parameter->ToObject();

    if (performAllChecks) {
      // first check if the object has a "toJSON" function
      if (o->Has(context.toJsonKey)) {
        // call it if yes
        v8::Handle<v8::Value> func = o->Get(context.toJsonKey);
        if (func->IsFunction()) {
          v8::Handle<v8::Function> toJson = v8::Handle<v8::Function>::Cast(func);

          v8::Handle<v8::Value> args;
          v8::Handle<v8::Value> converted = toJson->Call(o, 0, &args);

          if (!converted.IsEmpty()) {
            // return whatever toJSON returned
            v8::String::Utf8Value str(converted->ToString());

            if (*str == nullptr) {
              return TRI_ERROR_OUT_OF_MEMORY;
            }

            // this passes ownership for the utf8 string to the JSON object
            AddValuePair(context, attributeName, inObject, VPackValuePair(*str, str.length(), VPackValueType::String));
            return TRI_ERROR_NO_ERROR;
          }
        }

        // fall-through intentional
      }

      int hash = o->GetIdentityHash();

      if (context.seenHashes.find(hash) != context.seenHashes.end()) {
        // LOG(TRACE) << "found hash " << hash;

        for (auto& it : context.seenObjects) {
          if (parameter->StrictEquals(it)) {
            // object is recursive
            return TRI_ERROR_BAD_PARAMETER;
          }
        }
      } else {
        context.seenHashes.emplace(hash);
      }

      context.seenObjects.emplace_back(o);
    }

    v8::Handle<v8::Array> names = o->GetOwnPropertyNames();
    uint32_t const n = names->Length();

    AddValue(context, attributeName, inObject,
             VPackValue(VPackValueType::Object));

    for (uint32_t i = 0; i < n; ++i) {
      // process attribute name
      v8::Handle<v8::Value> key = names->Get(i);
      v8::String::Utf8Value str(key);

      if (*str == nullptr) {
        return TRI_ERROR_OUT_OF_MEMORY;
      }

      v8::Handle<v8::Value> value = o->Get(key);
      if (value->IsUndefined()) {
        // ignore object values which are undefined
        continue;
      }

      int res = V8ToVPack<performAllChecks>(context, value, *str, true);

      if (res != TRI_ERROR_NO_ERROR) {
        return res;
      }
    }

    if (performAllChecks) {
      context.seenObjects.pop_back();
      if (!context.keepTopLevelOpen || !context.seenObjects.empty()) {
        context.builder.close();
      }
    } else {
      if (!context.keepTopLevelOpen) {
        context.builder.close();
      }
    }
    return TRI_ERROR_NO_ERROR;
  }

  return TRI_ERROR_BAD_PARAMETER;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief convert a V8 value to VPack value
////////////////////////////////////////////////////////////////////////////////

int TRI_V8ToVPack(v8::Isolate* isolate, VPackBuilder& builder,
                  v8::Handle<v8::Value> const value, bool keepTopLevelOpen) {
  v8::HandleScope scope(isolate);
  TRI_GET_GLOBALS();
  BuilderContext context(isolate, builder, keepTopLevelOpen);
  TRI_GET_GLOBAL_STRING(ToJsonKey);
  context.toJsonKey = ToJsonKey;
  return V8ToVPack<true>(context, value, "", false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief convert a V8 value to VPack value, simplified version
/// this function assumes that the V8 object does not contain any cycles and
/// does not contain types such as Function, Date or RegExp
////////////////////////////////////////////////////////////////////////////////

int TRI_V8ToVPackSimple(v8::Isolate* isolate, arangodb::velocypack::Builder& builder,
                        v8::Handle<v8::Value> const value, bool keepTopLevelOpen) {
  v8::HandleScope scope(isolate);
  BuilderContext context(isolate, builder, keepTopLevelOpen);
  return V8ToVPack<false>(context, value, "", false);
}

