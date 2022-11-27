#include "prim-reconstruct.hh"

#include "prim-types.hh"
#include "str-util.hh"
#include "io-util.hh"
#include "tiny-format.hh"

#include "usdGeom.hh"
#include "usdSkel.hh"
#include "usdLux.hh"
#include "usdShade.hh"

#include "common-macros.inc"

// For PUSH_ERROR_AND_RETURN
#define PushError(s) if (err) { (*err) += s; }
#define PushWarn(s) if (warn) { (*warn) += s; }

// TODO:
// - [ ] PathList for `.connect` (e.g. string con.connect = [ </root>, </root.a>])
//

//
// NOTE:
//
// There are mainly 4 variant of Primtive property(attribute)
//
// - TypedAttribute<T> : Uniform only. `uniform T` or `uniform T var.connect`
// - TypedAttribute<Animatable<T>> : Varying. `T var`, `T var = val`, `T var.connect` or `T value.timeSamples`
// - optional<T> : For output attribute(Just author it. e.g. `float outputs:rgb`)
// - Relationship : Typeless relation(e.g. `rel material:binding`)

namespace tinyusdz {
namespace prim {

constexpr auto kTag = "[PrimReconstruct]";

constexpr auto kProxyPrim = "proxyPrim";
constexpr auto kMaterialBinding = "material:binding";
constexpr auto kMaterialBindingCorrection = "material:binding:correction";
constexpr auto kMaterialBindingPreview = "material:binding:preview";
constexpr auto kSkelSkeleton = "skel:skeleton";
constexpr auto kSkelAnimationSource = "skel:animationSource";
constexpr auto kSkelBlendShapes = "skel:blendShapes";
constexpr auto kSkelBlendShapeTargets = "skel:blendShapeTargets";

///
/// TinyUSDZ reconstruct some frequently used shaders(e.g. UsdPreviewSurface)
/// here, not in Tydra
///
template <typename T>
bool ReconstructShader(
    const PropertyMap &properties,
    const ReferenceList &references,
    T *out,
    std::string *warn,
    std::string *err);

namespace {


struct ParseResult
{
  enum class ResultCode
  {
    Success,
    Unmatched,
    AlreadyProcessed,
    TypeMismatch,
    VariabilityMismatch,
    ConnectionNotAllowed,
    InvalidConnection,
    InternalError,
  };

  ResultCode code;
  std::string err;
};

template<typename T>
static nonstd::optional<Animatable<T>> ConvertToAnimatable(const primvar::PrimVar &var)
{
  Animatable<T> dst;

  if (!var.is_valid()) {
    DCOUT("is_valid failed");
    return nonstd::nullopt;
  }

  if (var.is_scalar()) {

    if (auto pv = var.get_value<T>()) {
      dst.set(pv.value());

      return std::move(dst);
    }
  } else if (var.is_timesamples()) {
    for (size_t i = 0; i < var.ts_raw().size(); i++) {
      const value::TimeSamples::Sample &s = var.ts_raw().get_samples()[i];

      // Attribute Block?
      if (s.blocked) {
        dst.add_blocked_sample(s.t);
      } else if (auto pv = s.value.get_value<T>()) {
        dst.add_sample(s.t, pv.value());
      } else {
        // Type mismatch
        DCOUT(i << "/" << var.ts_raw().size() << " type mismatch.");
        return nonstd::nullopt;
      }
    }

    return std::move(dst);
  }

  DCOUT("???");
  return nonstd::nullopt;
}

// Require special treatment for Extent(float3[2])
template<>
nonstd::optional<Animatable<Extent>> ConvertToAnimatable(const primvar::PrimVar &var)
{
  Animatable<Extent> dst;

  if (!var.is_valid()) {
    DCOUT("is_valid failed");
    return nonstd::nullopt;
  }

  if (var.is_scalar()) {

    if (auto pv = var.get_value<std::vector<value::float3>>()) {
      if (pv.value().size() == 2) {
        Extent ext;
        ext.lower = pv.value()[0];
        ext.upper = pv.value()[1];

        dst.set(ext);

      } else {
        return nonstd::nullopt;
      }

      return std::move(dst);
    }
  } else if (var.is_timesamples()) {
    for (size_t i = 0; i < var.ts_raw().size(); i++) {
      const value::TimeSamples::Sample &s = var.ts_raw().get_samples()[i];

      // Attribute Block?
      if (s.blocked) {
        dst.add_blocked_sample(s.t);
      } else if (auto pv = s.value.get_value<std::vector<value::float3>>()) {
        if (pv.value().size() == 2) {
          Extent ext;
          ext.lower = pv.value()[0];
          ext.upper = pv.value()[1];
          dst.add_sample(s.t, ext);
        } else {
          DCOUT(i << "/" << var.ts_raw().size() << " array size mismatch.");
          return nonstd::nullopt;
        }
      } else {
        // Type mismatch
        DCOUT(i << "/" << var.ts_raw().size() << " type mismatch.");
        return nonstd::nullopt;
      }
    }

    return std::move(dst);
  }

  DCOUT("???");
  return nonstd::nullopt;
}

// For animatable attribute(`varying`)
template<typename T>
static ParseResult ParseTypedAttribute(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  const Property &prop,
  const std::string &name,
  TypedAttributeWithFallback<Animatable<T>> &target) /* out */
{
  ParseResult ret;

  if (prop_name.compare(name + ".connect") == 0) {
    std::string propname = removeSuffix(name, ".connect");
    if (table.count(propname)) {
      DCOUT("Already processed: " << prop_name);
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }
    if (prop.is_connection()) {
      if (auto pv = prop.get_relationTarget()) {
        target.set_connection(pv.value());
        //target.variability = prop.attrib.variability;
        target.metas() = prop.get_attribute().metas();
        table.insert(propname);
        ret.code = ParseResult::ResultCode::Success;
        DCOUT("Added as property with connection: " << propname);
        return ret;
      } else {
        ret.code = ParseResult::ResultCode::InvalidConnection;
        ret.err = "Connection target not found.";
        return ret;
      }
    } else {
      ret.code = ParseResult::ResultCode::InternalError;
      ret.err = "Internal error. Unsupported/Unimplemented property type.";
      return ret;
    }
  } else if (prop_name.compare(name) == 0) {
    if (table.count(name)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }

    const Attribute &attr = prop.get_attribute();

    if (prop.is_connection()) {
      if (attr.is_connection()) { // Ensure Attribute is also return true for is_connection
        target.set_connections(attr.connections());
        target.metas() = attr.metas();
        table.insert(prop_name);
        ret.code = ParseResult::ResultCode::Success;
      } else {
        ret.code = ParseResult::ResultCode::InternalError;
        ret.err = "Internal error. Invalid Property with Attribute connection.";
      }
      if (auto pv = prop.get_relationTarget()) {
        return ret;
      } else {
        return ret;
      }
    }


    std::string attr_type_name = attr.type_name();
    if ((value::TypeTraits<T>::type_name() == attr_type_name) || (value::TypeTraits<T>::underlying_type_name() == attr_type_name)) {
      if (prop.get_property_type() == Property::Type::EmptyAttrib) {
        DCOUT("Added prop with empty value: " << name);
        target.set_value_empty();
        target.metas() = attr.metas();
        table.insert(name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else if (prop.get_property_type() == Property::Type::Attrib) {

        DCOUT("Adding typed prop: " << name);

        if (attr.is_blocked()) {
          // e.g. "float radius = None"
          target.set_blocked(true);
        } else if (attr.variability() == Variability::Uniform) {
          // e.g. "float radius = 1.2"
          if (!attr.get_var().is_scalar()) {
            ret.code = ParseResult::ResultCode::VariabilityMismatch;
            ret.err = fmt::format("TimeSample value is assigned to `uniform` property `{}", name);
            return ret;
          }

          if (auto pv = attr.get_value<T>()) {
            target.set_value(pv.value());
          } else {
            ret.code = ParseResult::ResultCode::InternalError;
            ret.err = fmt::format("Failed to retrieve value with requested type.");
            return ret;
          }

        } else if (attr.get_var().is_timesamples()) {
          // e.g. "float radius.timeSamples = {0: 1.2, 1: 2.3}"

          Animatable<T> anim;
          if (auto av = ConvertToAnimatable<T>(attr.get_var())) {
            anim = av.value();
            target.set_value(anim);
          } else {
            // Conversion failed.
            DCOUT("ConvertToAnimatable failed.");
            ret.code = ParseResult::ResultCode::InternalError;
            ret.err = "Converting Attribute data failed. Maybe TimeSamples have values with different types?";
            return ret;
          }
        } else if (attr.get_var().is_scalar()) {
          if (auto pv = attr.get_value<T>()) {
            target.set_value(pv.value());
          } else {
            ret.code = ParseResult::ResultCode::InternalError;
            ret.err = "Invalid attribute value.";
            return ret;
          }
        } else {
          ret.code = ParseResult::ResultCode::InternalError;
          ret.err = "Invalid attribute value.";
          return ret;
        }

        target.metas() = attr.metas();
        table.insert(name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else {
        DCOUT("Invalid Property.type");
        ret.err = "Invalid Property type(internal error)";
        ret.code = ParseResult::ResultCode::InternalError;
        return ret;
      }
    } else {
      DCOUT("tyname = " << value::TypeTraits<T>::type_name() << ", attr.type = " << attr_type_name);
      ret.code = ParseResult::ResultCode::TypeMismatch;
      std::stringstream ss;
      ss  << "Property type mismatch. " << name << " expects type `"
              << value::TypeTraits<T>::type_name()
              << "` but defined as type `" << attr_type_name << "`";
      ret.err = ss.str();
      return ret;
    }
  }

  ret.code = ParseResult::ResultCode::Unmatched;
  return ret;
}

// For 'uniform' attribute
template<typename T>
static ParseResult ParseTypedAttribute(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  const Property &prop,
  const std::string &name,
  TypedAttributeWithFallback<T> &target) /* out */
{
  ParseResult ret;

  if (prop_name.compare(name + ".connect") == 0) {
    std::string propname = removeSuffix(name, ".connect");
    if (table.count(propname)) {
      DCOUT("Already processed: " << prop_name);
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }
    if (prop.is_connection()) {
      const Attribute &attr = prop.get_attribute();
      if (attr.is_connection()) {
        target.set_connections(attr.connections());
        //target.variability = prop.attrib.variability;
        target.metas() = prop.get_attribute().metas();
        table.insert(propname);
        ret.code = ParseResult::ResultCode::Success;
        DCOUT("Added as property with connection: " << propname);
        return ret;
      } else {
        ret.code = ParseResult::ResultCode::InvalidConnection;
        ret.err = "Connection target not found.";
        return ret;
      }
    } else {
      ret.code = ParseResult::ResultCode::InternalError;
      ret.err = "Internal error. Unsupported/Unimplemented property type.";
      return ret;
    }
  } else if (prop_name.compare(name) == 0) {
    if (table.count(name)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }

    if (prop.is_connection()) {
      const Attribute &attr = prop.get_attribute();
      if (attr.is_connection()) {
        target.set_connections(attr.connections());
        //target.variability = prop.attrib.variability;
        target.metas() = prop.get_attribute().metas();
        table.insert(prop_name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else {
        ret.code = ParseResult::ResultCode::InternalError;
        ret.err = "Internal error. Invalid property with connection.";
        return ret;
      }
    }

    const Attribute &attr = prop.get_attribute();

    std::string attr_type_name = attr.type_name();
    if ((value::TypeTraits<T>::type_name() == attr_type_name) || (value::TypeTraits<T>::underlying_type_name() == attr_type_name)) {
      if (prop.get_property_type() == Property::Type::EmptyAttrib) {
        DCOUT("Added prop with empty value: " << name);
        target.set_value_empty();
        target.metas() = attr.metas();
        table.insert(name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else if (prop.get_property_type() == Property::Type::Attrib) {
        DCOUT("Adding prop: " << name);

        if (prop.get_attribute().variability() != Variability::Uniform) {
          ret.code = ParseResult::ResultCode::VariabilityMismatch;
          ret.err = fmt::format("Attribute `{}` must be `uniform` variability.", name);
          return ret;
        }

        if (attr.is_blocked()) {
          target.set_blocked(true);
        } else if (attr.get_var().is_scalar()) {
          if (auto pv = attr.get_value<T>()) {
            target.set_value(pv.value());
          } else {
            ret.code = ParseResult::ResultCode::InternalError;
            ret.err = "Internal data corrupsed.";
            return ret;
          }
        } else {
          ret.code = ParseResult::ResultCode::VariabilityMismatch;
          ret.err = "TimeSample or corrupted value assigned to a property where `uniform` variability is set.";
          return ret;
        }

        target.metas() = attr.metas();
        table.insert(name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else {
        DCOUT("Invalid Property.type");
        ret.err = "Invalid Property type(internal error)";
        ret.code = ParseResult::ResultCode::InternalError;
        return ret;
      }
    } else {
      DCOUT("tyname = " << value::TypeTraits<T>::type_name() << ", attr.type = " << attr_type_name);
      ret.code = ParseResult::ResultCode::TypeMismatch;
      std::stringstream ss;
      ss  << "Property type mismatch. " << name << " expects type `"
              << value::TypeTraits<T>::type_name()
              << "` but defined as type `" << attr_type_name << "`";
      ret.err = ss.str();
      return ret;
    }
  }

  ret.code = ParseResult::ResultCode::Unmatched;
  return ret;
}

// For animatable attribute(`varying`)
template<typename T>
static ParseResult ParseTypedAttribute(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  const Property &prop,
  const std::string &name,
  TypedAttribute<Animatable<T>> &target) /* out */
{
  ParseResult ret;

  if (prop_name.compare(name + ".connect") == 0) {
    std::string propname = removeSuffix(name, ".connect");
    if (table.count(propname)) {
      DCOUT("Already processed: " << prop_name);
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }
    if (prop.is_connection()) {
      const Attribute &attr = prop.get_attribute();
      if (attr.is_connection()) {
        target.set_connections(attr.connections());
        //target.variability = prop.attrib.variability;
        target.metas() = prop.get_attribute().metas();
        table.insert(propname);
        ret.code = ParseResult::ResultCode::Success;
        DCOUT("Added as property with connection: " << propname);
        return ret;
      } else {
        ret.code = ParseResult::ResultCode::InvalidConnection;
        ret.err = "Connection target not found.";
        return ret;
      }
    } else {
      ret.code = ParseResult::ResultCode::InternalError;
      ret.err = "Internal error. Unsupported/Unimplemented property type.";
      return ret;
    }
  } else if (prop_name.compare(name) == 0) {
    if (table.count(name)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }

    if (prop.is_connection()) {
      const Attribute &attr = prop.get_attribute();
      if (attr.is_connection()) {
        target.set_connections(attr.connections());
        //target.variability = prop.attrib.variability;
        target.metas() = prop.get_attribute().metas();
        table.insert(prop_name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else {
        ret.code = ParseResult::ResultCode::InternalError;
        ret.err = "Internal error. Invalid property with connection.";
        return ret;
      }
    }

    const Attribute &attr = prop.get_attribute();

    std::string attr_type_name = attr.type_name();
    if ((value::TypeTraits<T>::type_name() == attr_type_name) || (value::TypeTraits<T>::underlying_type_name() == attr_type_name)) {
      if (prop.get_property_type() == Property::Type::EmptyAttrib) {
        DCOUT("Added prop with empty value: " << name);
        target.set_value_empty();
        target.metas() = attr.metas();
        table.insert(name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else if (prop.get_property_type() == Property::Type::Attrib) {

        DCOUT("Adding typed attribute: " << name);

        if (attr.is_blocked()) {
          // e.g. "float radius = None"
          target.set_blocked(true);
        } else if (attr.variability() == Variability::Uniform) {
          // e.g. "float radius = 1.2"
          if (!attr.get_var().is_scalar()) {
            ret.code = ParseResult::ResultCode::VariabilityMismatch;
            ret.err = fmt::format("TimeSample value is assigned to `uniform` property `{}", name);
            return ret;
          }

          if (auto pv = attr.get_value<T>()) {
            target.set_value(pv.value());
          } else {
            ret.code = ParseResult::ResultCode::InternalError;
            ret.err = fmt::format("Failed to retrieve value with requested type.");
            return ret;
          }

        } else if (attr.get_var().is_timesamples()) {
          // e.g. "float radius.timeSamples = {0: 1.2, 1: 2.3}"

          Animatable<T> anim;
          if (auto av = ConvertToAnimatable<T>(attr.get_var())) {
            anim = av.value();
            target.set_value(anim);
          } else {
            // Conversion failed.
            DCOUT("ConvertToAnimatable failed.");
            ret.code = ParseResult::ResultCode::InternalError;
            ret.err = "Converting Attribute data failed. Maybe TimeSamples have values with different types?";
            return ret;
          }
        } else if (attr.get_var().is_scalar()) {
          if (auto pv = attr.get_value<T>()) {
            target.set_value(pv.value());
          } else {
            ret.code = ParseResult::ResultCode::InternalError;
            ret.err = fmt::format("Failed to retrieve value with requested type.");
            return ret;
          }
        } else {
            ret.code = ParseResult::ResultCode::InternalError;
            ret.err = "Invalid or Unsupported attribute data.";
            return ret;
        }

        DCOUT("Added typed attribute: " << name);

        target.metas() = attr.metas();
        table.insert(name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else {
        DCOUT("Invalid Property.type");
        ret.err = "Invalid Property type(internal error)";
        ret.code = ParseResult::ResultCode::InternalError;
        return ret;
      }
    } else {
      DCOUT("tyname = " << value::TypeTraits<T>::type_name() << ", attr.type = " << attr_type_name);
      ret.code = ParseResult::ResultCode::TypeMismatch;
      std::stringstream ss;
      ss  << "Property type mismatch. " << name << " expects type `"
              << value::TypeTraits<T>::type_name()
              << "` but defined as type `" << attr_type_name << "`";
      ret.err = ss.str();
      return ret;
    }
  }

  ret.code = ParseResult::ResultCode::Unmatched;
  return ret;
}

// TODO: Unify code with TypedAttribute<Animatable<T>> variant
template<typename T>
static ParseResult ParseTypedAttribute(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  const Property &prop,
  const std::string &name,
  TypedAttribute<T> &target) /* out */
{
  ParseResult ret;

  DCOUT(fmt::format("prop name {}", prop_name));

  if (prop_name.compare(name + ".connect") == 0) {
    std::string propname = removeSuffix(name, ".connect");
    if (table.count(propname)) {
      DCOUT("Already processed: " << prop_name);
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }
    if (prop.is_connection()) {
      const Attribute &attr = prop.get_attribute();
      if (attr.is_connection()) {
        target.set_connections(attr.connections());
        //target.variability = prop.attrib.variability;
        target.metas() = prop.get_attribute().metas();
        table.insert(propname);
        ret.code = ParseResult::ResultCode::Success;
        DCOUT("Added as property with connection: " << propname);
        return ret;
      } else {
        ret.code = ParseResult::ResultCode::InvalidConnection;
        ret.err = "Connection target not found.";
        return ret;
      }
    } else {
      ret.code = ParseResult::ResultCode::InternalError;
      ret.err = "Internal error. Unsupported/Unimplemented property type.";
      return ret;
    }
  } else if (prop_name.compare(name) == 0) {
    DCOUT(fmt::format("prop name match {}", name));
    if (table.count(name)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }

    if (prop.is_connection()) {
      const Attribute &attr = prop.get_attribute();
      if (attr.is_connection()) {
        target.set_connections(attr.connections());
        //target.variability = prop.attrib.variability;
        target.metas() = prop.get_attribute().metas();
        table.insert(prop_name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else {
        ret.code = ParseResult::ResultCode::InternalError;
        ret.err = "Internal error. Invalid property with connection.";
        return ret;
      }
    }

    const Attribute &attr = prop.get_attribute();

    std::string attr_type_name = attr.type_name();
    DCOUT(fmt::format("prop name {}, type = {}", prop_name, attr_type_name));
    if ((value::TypeTraits<T>::type_name() == attr_type_name) || (value::TypeTraits<T>::underlying_type_name() == attr_type_name)) {
      if (prop.get_property_type() == Property::Type::EmptyAttrib) {
        DCOUT("Added prop with empty value: " << name);
        target.set_value_empty();
        target.metas() = attr.metas();
        table.insert(name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else if (prop.get_property_type() == Property::Type::Attrib) {

        DCOUT("Adding typed attribute: " << name);

        if (prop.get_attribute().variability() != Variability::Uniform) {
          ret.code = ParseResult::ResultCode::VariabilityMismatch;
          ret.err = fmt::format("Attribute `{}` must be `uniform` variability.", name);
          return ret;
        }

        if (attr.is_blocked()) {
          target.set_blocked(true);
        } else if (attr.get_var().is_scalar()) {
          if (auto pv = attr.get_value<T>()) {
            target.set_value(pv.value());
          } else {
            ret.code = ParseResult::ResultCode::VariabilityMismatch;
            ret.err = "Internal data corrupsed.";
            return ret;
          }
        } else {
          ret.code = ParseResult::ResultCode::VariabilityMismatch;
          ret.err = "TimeSample or corrupted value assigned to a property where `uniform` variability is set.";
          return ret;
        }

        target.metas() = attr.metas();
        table.insert(name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else {
        DCOUT("Invalid Property.type");
        ret.err = "Invalid Property type(internal error)";
        ret.code = ParseResult::ResultCode::InternalError;
        return ret;
      }
    } else {
      DCOUT("tyname = " << value::TypeTraits<T>::type_name() << ", attr.type = " << attr_type_name);
      ret.code = ParseResult::ResultCode::TypeMismatch;
      std::stringstream ss;
      ss  << "Property type mismatch. " << name << " expects type `"
              << value::TypeTraits<T>::type_name()
              << "` but defined as type `" << attr_type_name << "`";
      ret.err = ss.str();
      return ret;
    }
  }

  ret.code = ParseResult::ResultCode::Unmatched;
  return ret;
}

// Special case for Extent(float3[2]) type.
// TODO: Reuse code of ParseTypedAttribute as much as possible
static ParseResult ParseExtentAttribute(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  const Property &prop,
  const std::string &name,
  TypedAttribute<Animatable<Extent>> &target) /* out */
{
  ParseResult ret;

  if (prop_name.compare(name + ".connect") == 0) {
    std::string propname = removeSuffix(name, ".connect");
    if (table.count(propname)) {
      DCOUT("Already processed: " << prop_name);
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }
    if (prop.is_connection()) {
      const Attribute &attr = prop.get_attribute();
      if (attr.is_connection()) {
        target.set_connections(attr.connections());
        //target.variability = prop.attrib.variability;
        target.metas() = prop.get_attribute().metas();
        table.insert(propname);
        ret.code = ParseResult::ResultCode::Success;
        DCOUT("Added as property with connection: " << propname);
        return ret;
      } else {
        ret.code = ParseResult::ResultCode::InvalidConnection;
        ret.err = "Connection target not found.";
        return ret;
      }
    } else {
      ret.code = ParseResult::ResultCode::InternalError;
      ret.err = "Internal error. Unsupported/Unimplemented property type.";
      return ret;
    }
  } else if (prop_name.compare(name) == 0) {
    if (table.count(name)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }

    if (prop.is_connection()) {
      const Attribute &attr = prop.get_attribute();
      if (attr.is_connection()) {
        target.set_connections(attr.connections());
        //target.variability = prop.attrib.variability;
        target.metas() = prop.get_attribute().metas();
        table.insert(prop_name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else {
        ret.code = ParseResult::ResultCode::InternalError;
        ret.err = "Internal error. Invalid property with connection.";
        return ret;
      }
    }

    const Attribute &attr = prop.get_attribute();

    std::string attr_type_name = attr.type_name();
    if (prop.get_property_type() == Property::Type::EmptyAttrib) {
      DCOUT("Added prop with empty value: " << name);
      target.set_value_empty();
      target.metas() = attr.metas();
      table.insert(name);
      ret.code = ParseResult::ResultCode::Success;
      return ret;
    } else if (prop.get_property_type() == Property::Type::Attrib) {

      DCOUT("Adding typed attribute: " << name);

      if (attr.is_blocked()) {
        // e.g. "float3[] extent = None"
        target.set_blocked(true);
      } else if (attr.variability() == Variability::Uniform) {
        ret.code = ParseResult::ResultCode::VariabilityMismatch;
        ret.err = fmt::format("`extent` attribute is varying. `uniform` qualifier assigned to it.");
        return ret;
      } else if (attr.get_var().is_scalar()){
        if (auto pv = attr.get_value<std::vector<value::float3>>()) {
          if (pv.value().size() != 2) {
            ret.code = ParseResult::ResultCode::TypeMismatch;
            ret.err = fmt::format("`extent` must be `float3[2]`, but got array size {}", pv.value().size());
            return ret;
          }

          Extent ext;
          ext.lower = pv.value()[0];
          ext.upper = pv.value()[1];

          target.set_value(ext);

        } else {
          ret.code = ParseResult::ResultCode::TypeMismatch;
          ret.err = fmt::format("`extent` must be type `float3[]`, but got type `{}", attr.type_name());
          return ret;
        }

      } else if (attr.get_var().is_timesamples()) {
        // e.g. "float3[] extent.timeSamples = ..."

        Animatable<Extent> anim;
        if (auto av = ConvertToAnimatable<Extent>(attr.get_var())) {
          anim = av.value();
          target.set_value(anim);
        } else {
          // Conversion failed.
          DCOUT("ConvertToAnimatable failed.");
          ret.code = ParseResult::ResultCode::InternalError;
          ret.err = "Converting Attribute data failed. Maybe TimeSamples have values with different types or invalid array size?";
          return ret;
        }
      } else {
          ret.code = ParseResult::ResultCode::InternalError;
          ret.err = "Invalid or Unsupported Extent attribute value.";
          return ret;
      }

      DCOUT("Added Extent attribute: " << name);

      target.metas() = attr.metas();
      table.insert(name);
      ret.code = ParseResult::ResultCode::Success;
      return ret;
    } else {
      DCOUT("Invalid Property.type");
      ret.err = "Invalid Property type(internal error)";
      ret.code = ParseResult::ResultCode::InternalError;
      return ret;
    }
  }

  ret.code = ParseResult::ResultCode::Unmatched;
  return ret;
}


#if 0
template<typename T>
static ParseResult ParseTypedProperty(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  const Property &prop,
  const std::string &name,
  TypedProperty<T> &target) /* out */
{
  ParseResult ret;

  DCOUT("Parsing typed property: " << prop_name);

  if (prop_name.compare(name + ".connect") == 0) {
    std::string propname = removeSuffix(name, ".connect");
    if (table.count(propname)) {
      DCOUT("Already processed: " << prop_name);
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }
    if (prop.IsConnection()) {
      if (auto pv = prop.get_relationTarget()) {
        target.target = pv.value();
        target.variability = prop.attrib.variability;
        target.meta = prop.attrib.meta;
        table.insert(propname);
        ret.code = ParseResult::ResultCode::Success;
        DCOUT("Added as property with connection: " << propname);
        return ret;
      } else {
        ret.code = ParseResult::ResultCode::InvalidConnection;
        ret.err = "Connection target not found.";
        return ret;
      }
    } else {
      ret.code = ParseResult::ResultCode::InternalError;
      ret.err = "Internal error. Unsupported/Unimplemented property type.";
      return ret;
    }
  } else if (prop_name.compare(name) == 0) {
    if (table.count(name)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }
    const Attribute &attr = prop.attrib;

    DCOUT("prop is_rel = " << prop.is_relationship() << ", is_conn = " << prop.IsConnection());

    if (prop.IsConnection()) {
      if (auto pv = prop.get_relationTarget()) {
        target.target = pv.value();
        target.variability = prop.attrib.variability;
        target.meta = prop.attrib.meta;
        table.insert(prop_name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else {
        ret.code = ParseResult::ResultCode::InternalError;
        ret.err = "Internal error. Invalid property with connection.";
        return ret;
      }

    } else if (prop.IsAttrib()) {

      DCOUT("attrib.type = " << value::TypeTraits<T>::type_name() << ", attr.var.type= " << attr.type_name());

      std::string attr_type_name = attr.type_name();

      if ((value::TypeTraits<T>::type_name() == attr_type_name) || (value::TypeTraits<T>::underlying_type_name() == attr_type_name)) {
        if (prop.type == Property::Type::EmptyAttrib) {
          target.define_only = true;
          target.variability = attr.variability;
          target.meta = attr.meta;
          table.insert(name);
          ret.code = ParseResult::ResultCode::Success;
          return ret;
        } else if (prop.type == Property::Type::Attrib) {
          DCOUT("Adding prop: " << name);

          Animatable<T> anim;

          if (attr.blocked()) {
            anim.blocked = true;
          } else {
            if (auto av = ConvertToAnimatable<T>(attr.get_var())) {
              anim = av.value();
            } else {
              // Conversion failed.
              DCOUT("ConvertToAnimatable failed.");
              ret.code = ParseResult::ResultCode::InternalError;
              ret.err = "Converting Attribute data failed. Maybe TimeSamples have values with different types?";
              return ret;
            }
          }

          target.value = anim;
          target.variability = attr.variability;
          target.meta = attr.meta;
          table.insert(name);
          ret.code = ParseResult::ResultCode::Success;
          return ret;
        } else {
          DCOUT("Invalid Property.type");
          ret.err = "Invalid Property type(internal error)";
          ret.code = ParseResult::ResultCode::InternalError;
          return ret;
        }
      } else {
        DCOUT("tyname = " << value::TypeTraits<T>::type_name() << ", attr.type = " << attr_type_name);
        ret.code = ParseResult::ResultCode::TypeMismatch;
        std::stringstream ss;
        ss  << "Property type mismatch. " << name << " expects type `"
                << value::TypeTraits<T>::type_name()
                << "` but defined as type `" << attr_type_name << "`";
        ret.err = ss.str();
        return ret;
      }
    } else {
      ret.code = ParseResult::ResultCode::InternalError;
      ret.err = "Internal error. Unsupported/Unimplemented property type.";
      return ret;
    }
  }

  ret.code = ParseResult::ResultCode::Unmatched;
  return ret;
}
#endif


// Empty allowedTokens = allow all
template <class E, size_t N>
static nonstd::expected<bool, std::string> CheckAllowedTokens(
    const std::array<std::pair<E, const char *>, N> &allowedTokens,
    const std::string &tok) {
  if (allowedTokens.empty()) {
    return true;
  }

  for (size_t i = 0; i < N; i++) {
    if (tok.compare(std::get<1>(allowedTokens[i])) == 0) {
      return true;
    }
  }

  std::vector<std::string> toks;
  for (size_t i = 0; i < N; i++) {
    toks.push_back(std::get<1>(allowedTokens[i]));
  }

  std::string s = join(", ", tinyusdz::quote(toks));

  return nonstd::make_unexpected("Allowed tokens are [" + s + "] but got " +
                                 quote(tok) + ".");
};

// Allowed syntax:
//   "T varname"
template<typename T>
static ParseResult ParseShaderOutputTerminalAttribute(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  const Property &prop,
  const std::string &name,
  TypedTerminalAttribute<T> &target) /* out */
{
  ParseResult ret;

  if (prop_name.compare(name + ".connect") == 0) {
    ret.code = ParseResult::ResultCode::ConnectionNotAllowed;
    ret.err = "Connection is not allowed for output terminal attribute.";
    return ret;
  } else if (prop_name.compare(name) == 0) {
    if (table.count(name)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }

    if (prop.is_connection()) {
      ret.code = ParseResult::ResultCode::ConnectionNotAllowed;
      ret.err = "Connection is not allowed for output terminal attribute.";
      return ret;
    } else {

      const Attribute &attr = prop.get_attribute();

      std::string attr_type_name = attr.type_name();
      if (value::TypeTraits<T>::type_name() == attr_type_name) {
        if (prop.get_property_type() == Property::Type::EmptyAttrib) {
          // OK
          target.set_authored(true);
          target.metas() = prop.get_attribute().metas();
          table.insert(name);
          ret.code = ParseResult::ResultCode::Success;
          return ret;
        } else {
          DCOUT("Output Invalid Property.type");
          ret.err = "Invalid connection or value assigned for output terminal attribute.";
          ret.code = ParseResult::ResultCode::InvalidConnection;
          return ret;
        }
      } else {
        DCOUT("attr.type = " << attr_type_name);
        ret.code = ParseResult::ResultCode::TypeMismatch;
        ret.err = fmt::format("Property type mismatch. {} expects type `{}` but defined as type `{}`.", name, value::TypeTraits<T>::type_name(), attr_type_name);
        return ret;
      }
    }
  }

  ret.code = ParseResult::ResultCode::Unmatched;
  return ret;
}

// Allowed syntax:
//   "token outputs:surface"
//   "token outputs:surface.connect = </path/to/conn/>"
static ParseResult ParseShaderOutputProperty(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  const Property &prop,
  const std::string &name,
  nonstd::optional<Relationship> &target) /* out */
{
  ParseResult ret;

  if (prop_name.compare(name + ".connect") == 0) {
    std::string propname = removeSuffix(name, ".connect");
    if (table.count(propname)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }
    if (auto pv = prop.get_relationTarget()) {
      Relationship rel;
      rel.set(pv.value());
      rel.meta = prop.get_attribute().metas();
      target = rel;
      table.insert(propname);
      ret.code = ParseResult::ResultCode::Success;
      return ret;
    }
  } else if (prop_name.compare(name) == 0) {
    if (table.count(name)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }

    if (prop.is_connection()) {
      const Attribute &attr = prop.get_attribute();
      if (attr.is_connection()) {
        Relationship rel;
        std::vector<Path> conns = attr.connections();

        if (conns.size() == 0) {
          ret.code = ParseResult::ResultCode::InternalError;
          ret.err = "Invalid shader output attribute with connection. connection targetPath size is zero.";
          return ret;
        }

        if (conns.size() == 1) {
          rel.set(conns[0]);
        } else if (conns.size() > 1) {
          rel.set(conns);
        }

        rel.meta = prop.get_attribute().metas();
        target = rel;
        table.insert(prop_name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;

      } else {
        ret.code = ParseResult::ResultCode::InternalError;
        ret.err = "Invalid shader output attribute with connection.";
        return ret;
      }
    } else {

      const Attribute &attr = prop.get_attribute();

      std::string attr_type_name = attr.type_name();
      if (value::TypeTraits<value::token>::type_name() == attr_type_name) {
        if (prop.get_property_type() == Property::Type::EmptyAttrib) {
          Relationship rel;
          rel.set_novalue();
          rel.meta = prop.get_attribute().metas();
          table.insert(name);
          target = rel;
          ret.code = ParseResult::ResultCode::Success;
          return ret;
        } else {
          DCOUT("Output Invalid Property.type");
          ret.err = "Invalid connection or value assigned for output attribute.";
          ret.code = ParseResult::ResultCode::InvalidConnection;
          return ret;
        }
      } else {
        DCOUT("attr.type = " << attr.type_name());
        ret.code = ParseResult::ResultCode::TypeMismatch;
        std::stringstream ss;
        ss  << "Property type mismatch. " << name << " expects type `token` but defined as type `" << attr.type_name() << "`";
        ret.err = ss.str();
        return ret;
      }
    }
  }

  ret.code = ParseResult::ResultCode::Unmatched;
  return ret;
}

// Allowed syntax:
//   "token outputs:surface.connect = </path/to/conn/>"
static ParseResult ParseShaderInputConnectionProperty(std::set<std::string> &table, /* inout */
  const std::string prop_name,
  const Property &prop,
  const std::string &name,
  nonstd::optional<Connection<Path>> &target) /* out */
{
  ParseResult ret;
  ret.code = ParseResult::ResultCode::InternalError;

  if (prop_name.compare(name + ".connect") == 0) {
    std::string propname = removeSuffix(name, ".connect");
    if (table.count(propname)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }
    if (auto pv = prop.get_relationTarget()) {
      Connection<Path> conn;
      conn.target = pv.value();
      target = conn;
      /* conn.meta = prop.attrib.meta; */ // TODO
      table.insert(propname);
      ret.code = ParseResult::ResultCode::Success;
      return ret;
    } else {
      ret.code = ParseResult::ResultCode::InternalError;
      ret.err = "Property does not contain connectionPath.";
      return ret;
    }
  } else if (prop_name.compare(name) == 0) {
    if (table.count(name)) {
      ret.code = ParseResult::ResultCode::AlreadyProcessed;
      return ret;
    }

    if (prop.is_connection()) {
      const Attribute &attr = prop.get_attribute();
      if (attr.is_connection()) {
        Connection<Path> conn;

        if (attr.connections().size() == 1) {
          conn.target = attr.connections()[0];
        } else {
          ret.code = ParseResult::ResultCode::InternalError;
          ret.err = "Attribute does not contain connectionPath or multiple connetionPaths.";
          return ret;
        }
        target = conn;
        /* conn.meta = prop.attrib.meta; */ // TODO
        table.insert(prop_name);
        ret.code = ParseResult::ResultCode::Success;
        return ret;
      } else {
        ret.code = ParseResult::ResultCode::InternalError;
        ret.err = "Property does not contain connectionPath.";
        return ret;
      }
    } else {
      std::stringstream ss;
      ss  << "Property must have connection path.";
      ret.code = ParseResult::ResultCode::InternalError;
      ret.err = ss.str();
      return ret;
    }
  }

  ret.code = ParseResult::ResultCode::Unmatched;
  return ret;
}

#define PARSE_PROXY_PRIM_RELATION(__table, __prop, __ptarget) \
  if (prop.first == kProxyPrim) { \
    if (__table.count(kProxyPrim)) { \
       continue; \
    } \
    if (prop.second.is_relationship() && prop.second.is_empty()) { \
      PUSH_ERROR_AND_RETURN(fmt::format("`{}` must be a Relationship with Path target.", kProxyPrim)); \
    } \
    const Relationship &rel = prop.second.get_relationship(); \
    if (rel.is_path()) { \
      __ptarget->proxyPrim = rel; \
      table.insert(prop.first); \
      DCOUT("Added rel proxyPrim."); \
      continue; \
    } else { \
      PUSH_ERROR_AND_RETURN(fmt::format("`{}` target must be Path.", kProxyPrim)); \
    } \
  }

// Rel with single targetPath
#define PARSE_SINGLE_TARGET_PATH_RELATION(__table, __prop, __propname, __target) \
  if (prop.first == __propname) { \
    if (__table.count(__propname)) { \
       continue; \
    } \
    if (prop.second.is_relationship() && prop.second.is_empty()) { \
      PUSH_ERROR_AND_RETURN(fmt::format("`{}` must be a Relationship with Path target.", __propname)); \
    } \
    const Relationship &rel = prop.second.get_relationship(); \
    if (rel.is_path()) { \
      __target = rel; \
      table.insert(prop.first); \
      DCOUT("Added rel " << __propname); \
      continue; \
    } else if (rel.is_pathvector()) { \
      if (rel.targetPathVector.size() == 1) { \
        __target = rel; \
        table.insert(prop.first); \
        DCOUT("Added rel " << __propname); \
        continue; \
      } \
      PUSH_ERROR_AND_RETURN(fmt::format("`{}` target is empty or has mutiple Paths. Must be single Path.", __propname)); \
    } else { \
      PUSH_ERROR_AND_RETURN(fmt::format("`{}` target must be Path.", __propname)); \
    } \
  }

// Rel with targetPaths(single path or array of Paths)
#define PARSE_TARGET_PATHS_RELATION(__table, __prop, __propname, __target) \
  if (prop.first == __propname) { \
    if (__table.count(__propname)) { \
       continue; \
    } \
    if (!prop.second.is_relationship()) { \
      PUSH_ERROR_AND_RETURN(fmt::format("`{}` must be a Relationship", __propname)); \
    } \
    const Relationship &rel = prop.second.get_relationship(); \
    __target = rel; \
    table.insert(prop.first); \
    DCOUT("Added rel " << __propname); \
    continue; \
  }


#define PARSE_SHADER_TERMINAL_ATTRIBUTE(__table, __prop, __name, __klass, __target) { \
  ParseResult ret = ParseShaderOutputTerminalAttribute(__table, __prop.first, __prop.second, __name, __target); \
  if (ret.code == ParseResult::ResultCode::Success || ret.code == ParseResult::ResultCode::AlreadyProcessed) { \
    DCOUT("Added shader terminal attribute: " << __name); \
    continue; /* got it */\
  } else if (ret.code == ParseResult::ResultCode::Unmatched) { \
    /* go next */ \
  } else { \
    PUSH_ERROR_AND_RETURN(fmt::format("Parsing shader output property `{}` failed. Error: {}", __name, ret.err)); \
  } \
}

#define PARSE_SHADER_OUTPUT_PROPERTY(__table, __prop, __name, __klass, __target) { \
  ParseResult ret = ParseShaderOutputProperty(__table, __prop.first, __prop.second, __name, __target); \
  if (ret.code == ParseResult::ResultCode::Success || ret.code == ParseResult::ResultCode::AlreadyProcessed) { \
    DCOUT("Added shader output property: " << __name); \
    continue; /* got it */\
  } else if (ret.code == ParseResult::ResultCode::Unmatched) { \
    /* go next */ \
  } else { \
    PUSH_ERROR_AND_RETURN(fmt::format("Parsing shader output property `{}` failed. Error: {}", __name, ret.err)); \
  } \
}

#define PARSE_SHADER_INPUT_CONNECTION_PROPERTY(__table, __prop, __name, __klass, __target) { \
  ParseResult ret = ParseShaderInputConnectionProperty(__table, __prop.first, __prop.second, __name, __target); \
  if (ret.code == ParseResult::ResultCode::Success || ret.code == ParseResult::ResultCode::AlreadyProcessed) { \
    DCOUT("Added shader input connection: " << __name); \
    continue; /* got it */\
  } else if (ret.code == ParseResult::ResultCode::Unmatched) { \
    /* go next */ \
  } else { \
    PUSH_ERROR_AND_RETURN(fmt::format("Parsing shader property `{}` failed. Error: {}", __name, ret.err)); \
  } \
}

template <class E>
static nonstd::expected<bool, std::string> CheckAllowedTokens(
    const std::vector<std::pair<E, const char *>> &allowedTokens,
    const std::string &tok) {
  if (allowedTokens.empty()) {
    return true;
  }

  for (size_t i = 0; i < allowedTokens.size(); i++) {
    if (tok.compare(std::get<1>(allowedTokens[i])) == 0) {
      return true;
    }
  }

  std::vector<std::string> toks;
  for (size_t i = 0; i < allowedTokens.size(); i++) {
    toks.push_back(std::get<1>(allowedTokens[i]));
  }

  std::string s = join(", ", tinyusdz::quote(toks));

  return nonstd::make_unexpected("Allowed tokens are [" + s + "] but got " +
                                 quote(tok) + ".");
};

template <typename T>
nonstd::expected<T, std::string> EnumHandler(
    const std::string &prop_name, const std::string &tok,
    const std::vector<std::pair<T, const char *>> &enums) {
  auto ret = CheckAllowedTokens<T>(enums, tok);
  if (!ret) {
    return nonstd::make_unexpected(ret.error());
  }

  for (auto &item : enums) {
    if (tok == item.second) {
      return item.first;
    }
  }
  // Should never reach here, though.
  return nonstd::make_unexpected(
      quote(tok) + " is an invalid token for attribute `" + prop_name + "`");
}


} // namespace

#define PARSE_TYPED_ATTRIBUTE(__table, __prop, __name, __klass, __target) { \
  ParseResult ret = ParseTypedAttribute(__table, __prop.first, __prop.second, __name, __target); \
  if (ret.code == ParseResult::ResultCode::Success || ret.code == ParseResult::ResultCode::AlreadyProcessed) { \
    continue; /* got it */\
  } else if (ret.code == ParseResult::ResultCode::Unmatched) { \
    /* go next */ \
  } else { \
    PUSH_ERROR_AND_RETURN(fmt::format("Parsing attribute `{}` failed. Error: {}", __name, ret.err)); \
  } \
}

#define PARSE_EXTENT_ATTRIBUTE(__table, __prop, __name, __klass, __target) { \
  ParseResult ret = ParseExtentAttribute(__table, __prop.first, __prop.second, __name, __target); \
  if (ret.code == ParseResult::ResultCode::Success || ret.code == ParseResult::ResultCode::AlreadyProcessed) { \
    continue; /* got it */\
  } else if (ret.code == ParseResult::ResultCode::Unmatched) { \
    /* go next */ \
  } else { \
    PUSH_ERROR_AND_RETURN(fmt::format("Parsing attribute `extent` failed. Error: {}", ret.err)); \
  } \
}

template <typename EnumTy>
using EnumHandlerFun = std::function<nonstd::expected<EnumTy, std::string>(
    const std::string &)>;

static nonstd::expected<Axis, std::string> AxisEnumHandler(const std::string &tok) {
  using EnumTy = std::pair<Axis, const char *>;
  const std::vector<EnumTy> enums = {
      std::make_pair(Axis::X, "X"),
      std::make_pair(Axis::Y,
                     "Y"),
      std::make_pair(Axis::Z, "Z"),
  };
  return EnumHandler<Axis>("axis", tok, enums);
};

static nonstd::expected<Visibility, std::string> VisibilityEnumHandler(const std::string &tok) {
  using EnumTy = std::pair<Visibility, const char *>;
  const std::vector<EnumTy> enums = {
      std::make_pair(Visibility::Inherited, "inherited"),
      std::make_pair(Visibility::Invisible, "invisible"),
  };
  return EnumHandler<Visibility>("visilibity", tok, enums);
};

static nonstd::expected<Purpose, std::string> PurposeEnumHandler(const std::string &tok) {
  using EnumTy = std::pair<Purpose, const char *>;
  const std::vector<EnumTy> enums = {
      std::make_pair(Purpose::Default, "default"),
      std::make_pair(Purpose::Proxy, "proxy"),
      std::make_pair(Purpose::Render, "render"),
      std::make_pair(Purpose::Guide, "guide"),
  };
  return EnumHandler<Purpose>("purpose", tok, enums);
};

static nonstd::expected<Orientation, std::string> OrientationEnumHandler(const std::string &tok) {
  using EnumTy = std::pair<Orientation, const char *>;
  const std::vector<EnumTy> enums = {
      std::make_pair(Orientation::RightHanded, "rightHanded"),
      std::make_pair(Orientation::LeftHanded, "leftHanded"),
  };
  return EnumHandler<Orientation>("orientation", tok, enums);
};

#if 0
// Animatable enum
template<typename T, typename EnumTy>
nonstd::expected<bool, std::string> ParseEnumProperty(
  const std::string &prop_name,
  EnumHandlerFun<EnumTy> enum_handler,
  const Attribute &attr,
  TypedAttributeWithFallback<Animatable<T>> *result)
{
  if (!result) {
    return false;
  }

  if (attr.variability == Variability::Uniform) {
    if (attr.get_var().is_timesamples()) {
      return nonstd::make_unexpected(fmt::format("Property `{}` is defined as `uniform` variability but TimeSample value is assigned.", prop_name));
    }

    if (auto tok = attr.get_value<value::token>()) {
      auto e = enum_handler(tok.value().str());
      if (e) {
        (*result) = e.value();
        return true;
      } else {
        return nonstd::make_unexpected(fmt::format("({}) {}", value::TypeTraits<T>::type_name(), e.error()));
      }
    } else {
      return nonstd::make_unexpected(fmt::format("Property `{}` must be type `token`, but got type `{}`", prop_name, attr.type_name()));
    }


  } else {
    // uniform or TimeSamples
    if (attr.get_var().is_scalar()) {

      if (auto tok = attr.get_value<value::token>()) {
        auto e = enum_handler(tok.value().str());
        if (e) {
          (*result) = e.value();
          return true;
        } else {
          return nonstd::make_unexpected(fmt::format("({}) {}", value::TypeTraits<T>::type_name(), e.error()));
        }
      } else {
        return nonstd::make_unexpected(fmt::format("Property `{}` must be type `token`, but got type `{}`", prop_name, attr.type_name()));
      }
    } else if (attr.get_var().is_timesamples()) {
      size_t n = attr.get_var().num_timesamples();

      TypedTimeSamples<T> samples;

      for (size_t i = 0; i < n; i++) {

        double sample_time;

        if (auto pv = attr.get_var().get_ts_time(i)) {
          sample_time = pv.value();
        } else {
          // This should not happen.
          return nonstd::make_unexpected("Internal error.");
        }

        if (auto pv = attr.get_var().is_ts_value_blocked(i)) {
          if (pv.value() == true) {
            samples.AddBlockedSample(sample_time);
            continue;
          }
        } else {
          // This should not happen.
          return nonstd::make_unexpected("Internal error.");
        }

        if (auto tok = attr.get_var().get_ts_value<value::token>(i)) {
          auto e = enum_handler(tok.value().str());
          if (e) {
            samples.AddSample(sample_time, e.value());
          } else {
            return nonstd::make_unexpected(fmt::format("({}) {}", value::TypeTraits<T>::type_name(), e.error()));
          }
        } else {
          return nonstd::make_unexpected(fmt::format("Property `{}`'s TimeSample value must be type `token`, but got invalid type", prop_name));
        }
      }

      result->ts = samples;
      return true;

    } else {
      return nonstd::make_unexpected(fmt::format("Property `{}` has invalid value."));
    }

  }

  return false;
}
#endif


// TODO: TimeSamples
#define PARSE_ENUM_PROPETY(__table, __prop, __name, __enum_handler, __klass, \
                           __target) {                                      \
  if (__prop.first == __name) {                                              \
    if (__table.count(__name)) { continue; } \
    const Attribute &attr = __prop.second.get_attribute();                           \
    if (auto tok = attr.get_value<value::token>()) {                     \
      auto e = __enum_handler(tok.value().str());                            \
      if (e) {                                                               \
        __target = e.value();                                                \
        /* TODO: attr meta __target.meta = attr.meta;  */                    \
        __table.insert(__name);                                              \
      } else {                                                               \
        PUSH_ERROR_AND_RETURN("(" << value::TypeTraits<__klass>::type_name()  \
                                  << ") " << e.error());                     \
      }                                                                      \
    } else {                                                                 \
      PUSH_ERROR_AND_RETURN("(" << value::TypeTraits<__klass>::type_name()    \
                                << ") Property type mismatch. " << __name    \
                                << " must be type `token`, but got `"        \
                                << attr.type_name() << "`.");            \
    }                                                                        \
  } }


// Add custom property(including property with "primvars" prefix)
// Please call this macro after listing up all predefined property using
// `PARSE_PROPERTY` and `PARSE_ENUM_PROPETY`
#define ADD_PROPERTY(__table, __prop, __klass, __dst) {        \
  /* Check if the property name is a predefined property */  \
  if (!__table.count(__prop.first)) {                        \
    DCOUT("custom property added: name = " << __prop.first); \
    __dst[__prop.first] = __prop.second;                     \
    __table.insert(__prop.first);                            \
  } \
 }

// This code path should not be reached though.
#define PARSE_PROPERTY_END_MAKE_ERROR(__table, __prop) {                     \
  if (!__table.count(__prop.first)) {                              \
    PUSH_ERROR_AND_RETURN("Unsupported/unimplemented property: " + \
                          __prop.first);                           \
  } \
 }

// This code path should not be reached though.
#define PARSE_PROPERTY_END_MAKE_WARN(__table, __prop) { \
  if (!__table.count(__prop.first)) { \
    PUSH_WARN("Unsupported/unimplemented property: " + __prop.first); \
   } \
 }

bool ReconstructXformOpsFromProperties(
  std::set<std::string> &table, /* inout */
  const std::map<std::string, Property> &properties,
  std::vector<XformOp> *xformOps,
  std::string *err)
{

  constexpr auto kTranslate = "xformOp:translate";
  constexpr auto kTransform = "xformOp:transform";
  constexpr auto kScale = "xformOp:scale";
  constexpr auto kRotateX = "xformOp:rotateX";
  constexpr auto kRotateY = "xformOp:rotateY";
  constexpr auto kRotateZ = "xformOp:rotateZ";
  constexpr auto kRotateXYZ = "xformOp:rotateXYZ";
  constexpr auto kRotateXZY = "xformOp:rotateXZY";
  constexpr auto kRotateYXZ = "xformOp:rotateYXZ";
  constexpr auto kRotateYZX = "xformOp:rotateYZX";
  constexpr auto kRotateZXY = "xformOp:rotateZXY";
  constexpr auto kRotateZYX = "xformOp:rotateZYX";
  constexpr auto kOrient = "xformOp:orient";

  // false : no prefix found.
  // true : return suffix(first namespace ':' is ommited.).
  // - "" for prefix only "xformOp:translate"
  // - "blender:pivot" for "xformOp:translate:blender:pivot"
  auto SplitXformOpToken =
      [](const std::string &s,
         const std::string &prefix) -> nonstd::optional<std::string> {
    if (startsWith(s, prefix)) {
      if (s.compare(prefix) == 0) {
        // prefix only.
        return std::string();  // empty suffix
      } else {
        std::string suffix = removePrefix(s, prefix);
        DCOUT("suffix = " << suffix);
        if (suffix.length() == 1) {  // maybe namespace only.
          return nonstd::nullopt;
        }

        // remove namespace ':'
        if (suffix[0] == ':') {
          // ok
          suffix.erase(0, 1);
        } else {
          return nonstd::nullopt;
        }

        return std::move(suffix);
      }
    }

    return nonstd::nullopt;
  };


  // Lookup xform values from `xformOpOrder`
  // TODO: TimeSamples, Connection
  if (properties.count("xformOpOrder")) {
    // array of string
    auto prop = properties.at("xformOpOrder");

    if (prop.is_relationship()) {
      PUSH_ERROR_AND_RETURN("Relationship for `xformOpOrder` is not supported.");
    } else if (auto pv =
                   prop.get_attribute().get_value<std::vector<value::token>>()) {

      // 'uniform' check
      if (prop.get_attribute().variability() != Variability::Uniform) {
        PUSH_ERROR_AND_RETURN("`xformOpOrder` must have `uniform` variability.");
      }

      for (size_t i = 0; i < pv.value().size(); i++) {
        const auto &item = pv.value()[i];

        XformOp op;

        std::string tok = item.str();
        DCOUT("xformOp token = " << tok);

        if (startsWith(tok, "!resetXformStack!")) {
          if (tok.compare("!resetXformStack!") != 0) {
            PUSH_ERROR_AND_RETURN(
                "`!resetXformStack!` must be defined solely(not to be a prefix "
                "to \"xformOp:*\")");
          }

          if (i != 0) {
            PUSH_ERROR_AND_RETURN(
                "`!resetXformStack!` must appear at the first element of "
                "xformOpOrder list.");
          }

          op.op_type = XformOp::OpType::ResetXformStack;
          xformOps->emplace_back(op);

          // skip looking up property
          continue;
        }

        if (startsWith(tok, "!invert!")) {
          DCOUT("invert!");
          op.inverted = true;
          tok = removePrefix(tok, "!invert!");
          DCOUT("tok = " << tok);
        }

        auto it = properties.find(tok);
        if (it == properties.end()) {
          PUSH_ERROR_AND_RETURN("Property `" + tok + "` not found.");
        }
        if (it->second.is_connection()) {
          PUSH_ERROR_AND_RETURN(
              "Connection(.connect) of xformOp property is not yet supported: "
              "`" +
              tok + "`");
        }
        const Attribute &attr = it->second.get_attribute();

        // Check `xformOp` namespace
        if (auto xfm = SplitXformOpToken(tok, kTransform)) {
          op.op_type = XformOp::OpType::Transform;
          op.suffix = xfm.value();  // may contain nested namespaces

          if (attr.get_var().is_timesamples()) {
            op.set_timesamples(attr.get_var().ts_raw());
          } else if (auto pvd = attr.get_value<value::matrix4d>()) {
            op.set_value(pvd.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "`xformOp:transform` must be type `matrix4d`, but got type `" +
                attr.type_name() + "`.");
          }

        } else if (auto tx = SplitXformOpToken(tok, kTranslate)) {
          op.op_type = XformOp::OpType::Translate;
          op.suffix = tx.value();

          if (attr.get_var().is_timesamples()) {
            op.set_timesamples(attr.get_var().ts_raw());
          } else if (auto pvd = attr.get_value<value::double3>()) {
            op.set_value(pvd.value());
          } else if (auto pvf = attr.get_value<value::float3>()) {
            op.set_value(pvf.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "`xformOp:translate` must be type `double3` or `float3`, but "
                "got type `" +
                attr.type_name() + "`.");
          }
        } else if (auto scale = SplitXformOpToken(tok, kScale)) {
          op.op_type = XformOp::OpType::Scale;
          op.suffix = scale.value();

          if (attr.get_var().is_timesamples()) {
            op.set_timesamples(attr.get_var().ts_raw());
          } else if (auto pvd = attr.get_value<value::double3>()) {
            op.set_value(pvd.value());
          } else if (auto pvf = attr.get_value<value::float3>()) {
            op.set_value(pvf.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "`xformOp:scale` must be type `double3` or `float3`, but got "
                "type `" +
                attr.type_name() + "`.");
          }
        } else if (auto rotX = SplitXformOpToken(tok, kRotateX)) {
          op.op_type = XformOp::OpType::RotateX;
          op.suffix = rotX.value();

          if (attr.get_var().is_timesamples()) {
            op.set_timesamples(attr.get_var().ts_raw());
          } else if (auto pvd = attr.get_value<double>()) {
            op.set_value(pvd.value());
          } else if (auto pvf = attr.get_value<float>()) {
            op.set_value(pvf.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "`xformOp:rotateX` must be type `double` or `float`, but got "
                "type `" +
                attr.type_name() + "`.");
          }
        } else if (auto rotY = SplitXformOpToken(tok, kRotateY)) {
          op.op_type = XformOp::OpType::RotateY;
          op.suffix = rotX.value();

          if (attr.get_var().is_timesamples()) {
            op.set_timesamples(attr.get_var().ts_raw());
          } else if (auto pvd = attr.get_value<double>()) {
            op.set_value(pvd.value());
          } else if (auto pvf = attr.get_value<float>()) {
            op.set_value(pvf.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "`xformOp:rotateY` must be type `double` or `float`, but got "
                "type `" +
                attr.type_name() + "`.");
          }
        } else if (auto rotZ = SplitXformOpToken(tok, kRotateZ)) {
          op.op_type = XformOp::OpType::RotateY;
          op.suffix = rotZ.value();

          if (attr.get_var().is_timesamples()) {
            op.set_timesamples(attr.get_var().ts_raw());
          } else if (auto pvd = attr.get_value<double>()) {
            op.set_value(pvd.value());
          } else if (auto pvf = attr.get_value<float>()) {
            op.set_value(pvf.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "`xformOp:rotateZ` must be type `double` or `float`, but got "
                "type `" +
                attr.type_name() + "`.");
          }
        } else if (auto rotateXYZ = SplitXformOpToken(tok, kRotateXYZ)) {
          op.op_type = XformOp::OpType::RotateXYZ;
          op.suffix = rotateXYZ.value();

          if (attr.get_var().is_timesamples()) {
            op.set_timesamples(attr.get_var().ts_raw());
          } else if (auto pvd = attr.get_value<value::double3>()) {
            op.set_value(pvd.value());
          } else if (auto pvf = attr.get_value<value::float3>()) {
            op.set_value(pvf.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "`xformOp:rotateXYZ` must be type `double3` or `float3`, but got "
                "type `" +
                attr.type_name() + "`.");
          }
        } else if (auto rotateXZY = SplitXformOpToken(tok, kRotateXZY)) {
          op.op_type = XformOp::OpType::RotateXZY;
          op.suffix = rotateXZY.value();

          if (attr.get_var().is_timesamples()) {
            op.set_timesamples(attr.get_var().ts_raw());
          } else if (auto pvd = attr.get_value<value::double3>()) {
            op.set_value(pvd.value());
          } else if (auto pvf = attr.get_value<value::float3>()) {
            op.set_value(pvf.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "`xformOp:rotateXZY` must be type `double3` or `float3`, but got "
                "type `" +
                attr.type_name() + "`.");
          }
        } else if (auto rotateYXZ = SplitXformOpToken(tok, kRotateYXZ)) {
          op.op_type = XformOp::OpType::RotateYXZ;
          op.suffix = rotateYXZ.value();

          if (attr.get_var().is_timesamples()) {
            op.set_timesamples(attr.get_var().ts_raw());
          } else if (auto pvd = attr.get_value<value::double3>()) {
            op.set_value(pvd.value());
          } else if (auto pvf = attr.get_value<value::float3>()) {
            op.set_value(pvf.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "`xformOp:rotateYXZ` must be type `double3` or `float3`, but got "
                "type `" +
                attr.type_name() + "`.");
          }
        } else if (auto rotateYZX = SplitXformOpToken(tok, kRotateYZX)) {
          op.op_type = XformOp::OpType::RotateYZX;
          op.suffix = rotateYZX.value();

          if (attr.get_var().is_timesamples()) {
            op.set_timesamples(attr.get_var().ts_raw());
          } else if (auto pvd = attr.get_value<value::double3>()) {
            op.set_value(pvd.value());
          } else if (auto pvf = attr.get_value<value::float3>()) {
            op.set_value(pvf.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "`xformOp:rotateYZX` must be type `double3` or `float3`, but got "
                "type `" +
                attr.type_name() + "`.");
          }
        } else if (auto rotateZXY = SplitXformOpToken(tok, kRotateZXY)) {
          op.op_type = XformOp::OpType::RotateZXY;
          op.suffix = rotateZXY.value();

          if (attr.get_var().is_timesamples()) {
            op.set_timesamples(attr.get_var().ts_raw());
          } else if (auto pvd = attr.get_value<value::double3>()) {
            op.set_value(pvd.value());
          } else if (auto pvf = attr.get_value<value::float3>()) {
            op.set_value(pvf.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "`xformOp:rotateZXY` must be type `double3` or `float3`, but got "
                "type `" +
                attr.type_name() + "`.");
          }
        } else if (auto rotateZYX = SplitXformOpToken(tok, kRotateZYX)) {
          op.op_type = XformOp::OpType::RotateZYX;
          op.suffix = rotateZYX.value();

          if (attr.get_var().is_timesamples()) {
            op.set_timesamples(attr.get_var().ts_raw());
          } else if (auto pvd = attr.get_value<value::double3>()) {
            op.set_value(pvd.value());
          } else if (auto pvf = attr.get_value<value::float3>()) {
            op.set_value(pvf.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "`xformOp:rotateZYX` must be type `double3` or `float3`, but got "
                "type `" +
                attr.type_name() + "`.");
          }
        } else if (auto orient = SplitXformOpToken(tok, kOrient)) {
          op.op_type = XformOp::OpType::Orient;
          op.suffix = orient.value();

          if (attr.get_var().is_timesamples()) {
            op.set_timesamples(attr.get_var().ts_raw());
          } else if (auto pvd = attr.get_value<value::quatf>()) {
            op.set_value(pvd.value());
          } else if (auto pvf = attr.get_value<value::quatd>()) {
            op.set_value(pvf.value());
          } else {
            PUSH_ERROR_AND_RETURN(
                "`xformOp:orient` must be type `quatf` or `quatd`, but got "
                "type `" +
                attr.type_name() + "`.");
          }
        } else {
          PUSH_ERROR_AND_RETURN(
              "token for xformOpOrder must have namespace `xformOp:***`, or .");
        }

        xformOps->emplace_back(op);
        table.insert(tok);
      }

    } else {
      PUSH_ERROR_AND_RETURN(
          "`xformOpOrder` must be type `token[]` but got type `"
          << prop.get_attribute().type_name() << "`.");
    }
  }

  table.insert("xformOpOrder");
  return true;
}


template <>
bool ReconstructPrim<Xform>(
    const PropertyMap &properties,
    const ReferenceList &references,
    Xform *xform,
    std::string *warn,
    std::string *err) {

  (void)references;
  (void)warn;

  std::set<std::string> table;
  if (!prim::ReconstructXformOpsFromProperties(table, properties, &xform->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBinding, xform->materialBinding)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kProxyPrim, xform->proxyPrim)
    PARSE_ENUM_PROPETY(table, prop, "visibility", VisibilityEnumHandler, Xform,
                   xform->visibility)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, Xform,
                       xform->purpose)
    PARSE_ENUM_PROPETY(table, prop, "orientation", OrientationEnumHandler, Xform,
                       xform->orientation)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", Xform, xform->extent)
    ADD_PROPERTY(table, prop, Xform, xform->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<Model>(
    const PropertyMap &properties,
    const ReferenceList &references,
    Model *model,
    std::string *warn,
    std::string *err) {
  DCOUT("Model ");
  (void)references;
  (void)model;
  (void)err;

  std::set<std::string> table;
  for (const auto &prop : properties) {
    ADD_PROPERTY(table, prop, Model, model->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<Scope>(
    const PropertyMap &properties,
    const ReferenceList &references,
    Scope *scope,
    std::string *warn,
    std::string *err) {
  // `Scope` is just a namespace in scene graph(no node xform)

  (void)references;
  (void)scope;
  (void)err;

  DCOUT("Scope");
  std::set<std::string> table;
  for (const auto &prop : properties) {
    ADD_PROPERTY(table, prop, Scope, scope->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<SkelRoot>(
    const PropertyMap &properties,
    const ReferenceList &references,
    SkelRoot *root,
    std::string *warn,
    std::string *err) {

  (void)references;

  std::set<std::string> table;
  if (!prim::ReconstructXformOpsFromProperties(table, properties, &root->xformOps, err)) {
    return false;
  }

  // SkelRoot is something like a grouping node, having 1 Skeleton and possibly?
  // multiple Prim hierarchy containing GeomMesh.
  // No specific properties for SkelRoot(AFAIK)

  // custom props only
  for (const auto &prop : properties) {
    ADD_PROPERTY(table, prop, SkelRoot, root->props)
    PARSE_ENUM_PROPETY(table, prop, "visibility", VisibilityEnumHandler, SkelRoot,
                   root->visibility)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, SkelRoot,
                       root->purpose)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", SkelRoot, root->extent)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<Skeleton>(
    const PropertyMap &properties,
    const ReferenceList &references,
    Skeleton *skel,
    std::string *warn,
    std::string *err) {

  (void)warn;
  (void)references;

  std::set<std::string> table;
  for (auto &prop : properties) {

    // SkelBindingAPI
    if (prop.first == kSkelAnimationSource) {

      // Must be relation of type Path.
      if (prop.second.is_relationship() && prop.second.get_relationship().is_path()) {
        {
          const Relationship &rel = prop.second.get_relationship();
          if (rel.is_path()) {
            skel->animationSource = rel;
            table.insert(kSkelAnimationSource);
          } else {
            PUSH_ERROR_AND_RETURN("`" << kSkelAnimationSource << "` target must be Path.");
          }
        }
      } else {
        PUSH_ERROR_AND_RETURN(
            "`" << kSkelAnimationSource << "` must be a Relationship with Path target.");
      }
    }

    //

    PARSE_TYPED_ATTRIBUTE(table, prop, "bindTransforms", Skeleton, skel->bindTransforms)
    PARSE_TYPED_ATTRIBUTE(table, prop, "joints", Skeleton, skel->joints)
    PARSE_TYPED_ATTRIBUTE(table, prop, "jointNames", Skeleton, skel->jointNames)
    PARSE_TYPED_ATTRIBUTE(table, prop, "restTransforms", Skeleton, skel->restTransforms)
    PARSE_ENUM_PROPETY(table, prop, "visibility", VisibilityEnumHandler, Skeleton,
                   skel->visibility)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, Skeleton,
                       skel->purpose)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", Skeleton, skel->extent)
    ADD_PROPERTY(table, prop, Skeleton, skel->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

  // usdview and Houdini USD importer expects both `bindTransforms` and `restTransforms` are authored in USD
  if (!table.count("bindTransforms")) {
    // usdview and Houdini allow `bindTransforms` is not authord in USD, but it cannot compute skinning correctly without it,
    // so report an error in TinyUSDZ for a while.
    PUSH_ERROR_AND_RETURN_TAG(kTag, "`bindTransforms` is missing in Skeleton. Currently TinyUSDZ expects `bindTransforms` must exist in Skeleton.");
  }

  if (!table.count("restTransforms")) {
    // usdview and Houdini allow `restTransforms` is not authord in USD(usdview warns it), but it cannot compute skinning correctly without it,
    // (even SkelAnimation supplies trasnforms for all joints)
    // so report an error in TinyUSDZ for a while.
    PUSH_ERROR_AND_RETURN_TAG(kTag, "`restTransforms`(local joint matrices at rest state) is missing in Skeleton. Currently TinyUSDZ expects `restTransforms` must exist in Skeleton.");
  }

  // len(bindTransforms) must be equal to len(restTransforms)
  // TODO: Support connection
  {
    bool valid = false;
    if (auto bt = skel->bindTransforms.get_value()) {
      if (auto rt = skel->restTransforms.get_value()) {
        if (bt.value().size() == rt.value().size()) {
          // ok
          valid = true;
        }
      }
    }

    if (!valid) {
      PUSH_ERROR_AND_RETURN_TAG(kTag, "Array length must be same for `bindTransforms` and `restTransforms`.");
    }
  }

  return true;
}

template <>
bool ReconstructPrim<SkelAnimation>(
    const PropertyMap &properties,
    const ReferenceList &references,
    SkelAnimation *skelanim,
    std::string *warn,
    std::string *err) {

  (void)warn;
  (void)references;
  std::set<std::string> table;
  for (auto &prop : properties) {
    PARSE_TYPED_ATTRIBUTE(table, prop, "joints", SkelAnimation, skelanim->joints)
    PARSE_TYPED_ATTRIBUTE(table, prop, "translations", SkelAnimation, skelanim->translations)
    PARSE_TYPED_ATTRIBUTE(table, prop, "rotations", SkelAnimation, skelanim->rotations)
    PARSE_TYPED_ATTRIBUTE(table, prop, "scales", SkelAnimation, skelanim->scales)
    PARSE_TYPED_ATTRIBUTE(table, prop, "blendShapes", SkelAnimation, skelanim->blendShapes)
    PARSE_TYPED_ATTRIBUTE(table, prop, "blendShapeWeights", SkelAnimation, skelanim->blendShapeWeights)
    ADD_PROPERTY(table, prop, Skeleton, skelanim->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<BlendShape>(
    const PropertyMap &properties,
    const ReferenceList &references,
    BlendShape *bs,
    std::string *warn,
    std::string *err) {
  (void)warn;
  (void)references;

  DCOUT("Reconstruct BlendShape");

  constexpr auto kOffsets = "offsets";
  constexpr auto kNormalOffsets = "normalOffsets";
  constexpr auto kPointIndices = "pointIndices";

  std::set<std::string> table;
  for (auto &prop : properties) {
    PARSE_TYPED_ATTRIBUTE(table, prop, kOffsets, BlendShape, bs->offsets)
    PARSE_TYPED_ATTRIBUTE(table, prop, kNormalOffsets, BlendShape, bs->normalOffsets)
    PARSE_TYPED_ATTRIBUTE(table, prop, kPointIndices, BlendShape, bs->pointIndices)
    ADD_PROPERTY(table, prop, Skeleton, bs->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

#if 0 // TODO: Check required properties exist in strict mode.
  // `offsets` and `normalOffsets` are required property
  if (!table.count(kOffsets)) {
    PUSH_ERROR_AND_RETURN("`offsets` property is missing. `uniform vector3f[] offsets` is a required property.");
  }
  if (!table.count(kNormalOffsets)) {
    PUSH_ERROR_AND_RETURN("`normalOffsets` property is missing. `uniform vector3f[] normalOffsets` is a required property.");
  }
#endif

  return true;
}

template <>
bool ReconstructPrim(
    const PropertyMap &properties,
    const ReferenceList &references,
    GPrim *gprim,
    std::string *warn,
    std::string *err) {
  (void)gprim;
  (void)err;

  (void)references;
  (void)properties;

  PUSH_WARN("TODO: GPrim");

  return true;
}

template <>
bool ReconstructPrim(
    const PropertyMap &properties,
    const ReferenceList &references,
    GeomBasisCurves *curves,
    std::string *warn,
    std::string *err) {
  (void)references;

  DCOUT("GeomBasisCurves");

  auto BasisHandler = [](const std::string &tok)
      -> nonstd::expected<GeomBasisCurves::Basis, std::string> {
    using EnumTy = std::pair<GeomBasisCurves::Basis, const char *>;
    const std::vector<EnumTy> enums = {
        std::make_pair(GeomBasisCurves::Basis::Bezier, "bezier"),
        std::make_pair(GeomBasisCurves::Basis::Bspline, "bspline"),
        std::make_pair(GeomBasisCurves::Basis::CatmullRom, "catmullRom"),
    };

    return EnumHandler<GeomBasisCurves::Basis>("basis", tok, enums);
  };

  auto TypeHandler = [](const std::string &tok)
      -> nonstd::expected<GeomBasisCurves::Type, std::string> {
    using EnumTy = std::pair<GeomBasisCurves::Type, const char *>;
    const std::vector<EnumTy> enums = {
        std::make_pair(GeomBasisCurves::Type::Cubic, "cubic"),
        std::make_pair(GeomBasisCurves::Type::Linear, "linear"),
    };

    return EnumHandler<GeomBasisCurves::Type>("type", tok, enums);
  };

  auto WrapHandler = [](const std::string &tok)
      -> nonstd::expected<GeomBasisCurves::Wrap, std::string> {
    using EnumTy = std::pair<GeomBasisCurves::Wrap, const char *>;
    const std::vector<EnumTy> enums = {
        std::make_pair(GeomBasisCurves::Wrap::Nonperiodic, "nonperiodic"),
        std::make_pair(GeomBasisCurves::Wrap::Periodic, "periodic"),
        std::make_pair(GeomBasisCurves::Wrap::Pinned, "periodic"),
    };

    return EnumHandler<GeomBasisCurves::Wrap>("wrap", tok, enums);
  };

  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &curves->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    PARSE_TYPED_ATTRIBUTE(table, prop, "curveVertexCounts", GeomBasisCurves,
                         curves->curveVertexCounts)
    PARSE_TYPED_ATTRIBUTE(table, prop, "points", GeomBasisCurves, curves->points)
    PARSE_TYPED_ATTRIBUTE(table, prop, "velocities", GeomBasisCurves,
                          curves->velocities)
    PARSE_TYPED_ATTRIBUTE(table, prop, "normals", GeomBasisCurves,
                  curves->normals)
    PARSE_TYPED_ATTRIBUTE(table, prop, "accelerations", GeomBasisCurves,
                 curves->accelerations)
    PARSE_TYPED_ATTRIBUTE(table, prop, "widths", GeomBasisCurves, curves->widths)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, GeomBasisCurves,
                       curves->purpose)
    PARSE_ENUM_PROPETY(table, prop, "type", TypeHandler, GeomBasisCurves,
                       curves->type)
    PARSE_ENUM_PROPETY(table, prop, "basis", BasisHandler, GeomBasisCurves,
                       curves->basis)
    PARSE_ENUM_PROPETY(table, prop, "wrap", WrapHandler, GeomBasisCurves,
                       curves->wrap)

    ADD_PROPERTY(table, prop, GeomBasisCurves, curves->props)

    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<SphereLight>(
    const PropertyMap &properties,
    const ReferenceList &references,
    SphereLight *light,
    std::string *warn,
    std::string *err) {

  (void)references;

  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &light->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    // PARSE_PROPERTY(prop, "inputs:colorTemperature", light->colorTemperature)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:color", SphereLight, light->color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:radius", SphereLight, light->radius)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:intensity", SphereLight,
                   light->intensity)
    PARSE_ENUM_PROPETY(table, prop, "visibility", VisibilityEnumHandler, SphereLight,
                   light->visibility)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, SphereLight,
                       light->purpose)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", SphereLight, light->extent)
    ADD_PROPERTY(table, prop, SphereLight, light->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<RectLight>(
    const PropertyMap &properties,
    const ReferenceList &references,
    RectLight *light,
    std::string *warn,
    std::string *err) {

  (void)references;

  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &light->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    // PARSE_PROPERTY(prop, "inputs:colorTemperature", light->colorTemperature)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:texture:file", UsdUVTexture, light->file)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:color", RectLight, light->color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:height", RectLight, light->height)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:width", RectLight, light->width)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:intensity", RectLight,
                   light->intensity)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", RectLight, light->extent)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, RectLight,
                       light->purpose)
    ADD_PROPERTY(table, prop, SphereLight, light->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<DiskLight>(
    const PropertyMap &properties,
    const ReferenceList &references,
    DiskLight *light,
    std::string *warn,
    std::string *err) {

  (void)references;

  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &light->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    // PARSE_PROPERTY(prop, "inputs:colorTemperature", light->colorTemperature)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:radius", DiskLight, light->radius)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", DiskLight, light->extent)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, DiskLight,
                       light->purpose)
    ADD_PROPERTY(table, prop, DiskLight, light->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<CylinderLight>(
    const PropertyMap &properties,
    const ReferenceList &references,
    CylinderLight *light,
    std::string *warn,
    std::string *err) {

  (void)references;

  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &light->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    // PARSE_PROPERTY(prop, "inputs:colorTemperature", light->colorTemperature)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:length", CylinderLight, light->length)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:radius", CylinderLight, light->radius)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", CylinderLight, light->extent)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, CylinderLight,
                       light->purpose)
    ADD_PROPERTY(table, prop, SphereLight, light->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<DistantLight>(
    const PropertyMap &properties,
    const ReferenceList &references,
    DistantLight *light,
    std::string *warn,
    std::string *err) {

  (void)references;

  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &light->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    // PARSE_PROPERTY(prop, "inputs:colorTemperature", light->colorTemperature)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:angle", DistantLight, light->angle)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, DistantLight,
                       light->purpose)
    ADD_PROPERTY(table, prop, SphereLight, light->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<DomeLight>(
    const PropertyMap &properties,
    const ReferenceList &references,
    DomeLight *light,
    std::string *warn,
    std::string *err) {

  (void)references;

  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &light->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    PARSE_TYPED_ATTRIBUTE(table, prop, "guideRadius", DomeLight, light->guideRadius)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:diffuse", DomeLight, light->diffuse)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specular", DomeLight,
                   light->specular)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:colorTemperature", DomeLight,
                   light->colorTemperature)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:color", DomeLight, light->color)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:intensity", DomeLight,
                   light->intensity)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, DomeLight,
                       light->purpose)
    ADD_PROPERTY(table, prop, DomeLight, light->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  DCOUT("Implement DomeLight");
  return true;
}

template <>
bool ReconstructPrim<GeomSphere>(
    const PropertyMap &properties,
    const ReferenceList &references,
    GeomSphere *sphere,
    std::string *warn,
    std::string *err) {

  (void)warn;
  (void)references;

  DCOUT("Reconstruct Sphere.");

#if 0 //  TODO
  //
  // Resolve prepend references
  //
  for (const auto &ref : references) {
    DCOUT("asset_path = '" + std::get<1>(ref).asset_path + "'\n");

    if ((std::get<0>(ref) == tinyusdz::ListEditQual::ResetToExplicit) ||
        (std::get<0>(ref) == tinyusdz::ListEditQual::Prepend)) {
      const Reference &asset_ref = std::get<1>(ref);

      std::string filepath = asset_ref.asset_path;
      if (!io::IsAbsPath(filepath)) {
        filepath = io::JoinPath(_base_dir, filepath);
      }

      if (_reference_cache.count(filepath)) {
        DCOUT("Got a cache: filepath = " + filepath);

        const auto root_nodes = _reference_cache.at(filepath);
        const GPrim &prim = std::get<1>(root_nodes)[std::get<0>(root_nodes)];

        for (const auto &prop : prim.props) {
          (void)prop;
#if 0
          if (auto attr = nonstd::get_if<Attribute>(&prop.second)) {
            if (prop.first == "radius") {
              if (auto p = value::as_basic<double>(&attr->var)) {
                SDCOUT << "prepend reference radius = " << (*p) << "\n";
                sphere->radius = *p;
              }
            }
          }
#endif
        }
      }
    }
  }
#endif

  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &sphere->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    DCOUT("prop: " << prop.first);
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBinding, sphere->materialBinding)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingCorrection, sphere->materialBindingCorrection)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingPreview, sphere->materialBindingPreview)
    PARSE_TYPED_ATTRIBUTE(table, prop, "radius", GeomSphere, sphere->radius)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", GeomSphere, sphere->extent)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, GeomSphere,
                       sphere->purpose)
    ADD_PROPERTY(table, prop, GeomSphere, sphere->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

#if 0 // TODO
  //
  // Resolve append references
  // (Overwrite variables with the referenced one).
  //
  for (const auto &ref : references) {
    if (std::get<0>(ref) == tinyusdz::ListEditQual::Append) {
      const Reference &asset_ref = std::get<1>(ref);

      std::string filepath = asset_ref.asset_path;
      if (!io::IsAbsPath(filepath)) {
        filepath = io::JoinPath(_base_dir, filepath);
      }

      if (_reference_cache.count(filepath)) {
        DCOUT("Got a cache: filepath = " + filepath);

        const auto root_nodes = _reference_cache.at(filepath);
        const GPrim &prim = std::get<1>(root_nodes)[std::get<0>(root_nodes)];

        for (const auto &prop : prim.props) {
          (void)prop;
          // if (auto attr = nonstd::get_if<Attribute>(&prop.second)) {
          //   if (prop.first == "radius") {
          //     if (auto p = value::as_basic<double>(&attr->var)) {
          //       SDCOUT << "append reference radius = " << (*p) << "\n";
          //       sphere->radius = *p;
          //     }
          //   }
          // }
        }
      }
    }
  }
#endif

  return true;
}

template <>
bool ReconstructPrim<GeomPoints>(
    const PropertyMap &properties,
    const ReferenceList &references,
    GeomPoints *points,
    std::string *warn,
    std::string *err) {

  (void)warn;
  (void)references;

  DCOUT("Reconstruct Points.");

#if 0 //  TODO
  //
  // Resolve prepend references
  //
  for (const auto &ref : references) {
    DCOUT("asset_path = '" + std::get<1>(ref).asset_path + "'\n");

    if ((std::get<0>(ref) == tinyusdz::ListEditQual::ResetToExplicit) ||
        (std::get<0>(ref) == tinyusdz::ListEditQual::Prepend)) {
      const Reference &asset_ref = std::get<1>(ref);

      std::string filepath = asset_ref.asset_path;
      if (!io::IsAbsPath(filepath)) {
        filepath = io::JoinPath(_base_dir, filepath);
      }

      if (_reference_cache.count(filepath)) {
        DCOUT("Got a cache: filepath = " + filepath);

        const auto root_nodes = _reference_cache.at(filepath);
        const GPrim &prim = std::get<1>(root_nodes)[std::get<0>(root_nodes)];

        for (const auto &prop : prim.props) {
          (void)prop;
#if 0
          if (auto attr = nonstd::get_if<Attribute>(&prop.second)) {
            if (prop.first == "radius") {
              if (auto p = value::as_basic<double>(&attr->var)) {
                SDCOUT << "prepend reference radius = " << (*p) << "\n";
                sphere->radius = *p;
              }
            }
          }
#endif
        }
      }
    }
  }
#endif

  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &points->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    DCOUT("prop: " << prop.first);
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBinding, points->materialBinding)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingCorrection, points->materialBindingCorrection)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingPreview, points->materialBindingPreview)
    PARSE_TYPED_ATTRIBUTE(table, prop, "points", GeomPoints, points->points)
    PARSE_TYPED_ATTRIBUTE(table, prop, "normals", GeomPoints, points->normals)
    PARSE_TYPED_ATTRIBUTE(table, prop, "widths", GeomPoints, points->widths)
    PARSE_TYPED_ATTRIBUTE(table, prop, "ids", GeomPoints, points->ids)
    PARSE_TYPED_ATTRIBUTE(table, prop, "velocities", GeomPoints, points->velocities)
    PARSE_TYPED_ATTRIBUTE(table, prop, "accelerations", GeomPoints, points->accelerations)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", GeomPoints, points->extent)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, GeomPoints,
                       points->purpose)
    ADD_PROPERTY(table, prop, GeomSphere, points->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

#if 0 // TODO
  //
  // Resolve append references
  // (Overwrite variables with the referenced one).
  //
  for (const auto &ref : references) {
    if (std::get<0>(ref) == tinyusdz::ListEditQual::Append) {
      const Reference &asset_ref = std::get<1>(ref);

      std::string filepath = asset_ref.asset_path;
      if (!io::IsAbsPath(filepath)) {
        filepath = io::JoinPath(_base_dir, filepath);
      }

      if (_reference_cache.count(filepath)) {
        DCOUT("Got a cache: filepath = " + filepath);

        const auto root_nodes = _reference_cache.at(filepath);
        const GPrim &prim = std::get<1>(root_nodes)[std::get<0>(root_nodes)];

        for (const auto &prop : prim.props) {
          (void)prop;
          // if (auto attr = nonstd::get_if<Attribute>(&prop.second)) {
          //   if (prop.first == "radius") {
          //     if (auto p = value::as_basic<double>(&attr->var)) {
          //       SDCOUT << "append reference radius = " << (*p) << "\n";
          //       sphere->radius = *p;
          //     }
          //   }
          // }
        }
      }
    }
  }
#endif

  return true;
}

template <>
bool ReconstructPrim<GeomCone>(
    const PropertyMap &properties,
    const ReferenceList &references,
    GeomCone *cone,
    std::string *warn,
    std::string *err) {

  (void)warn;
  (void)references;

  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &cone->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    DCOUT("prop: " << prop.first);
    PARSE_PROXY_PRIM_RELATION(table, prop, cone)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBinding, cone->materialBinding)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingCorrection, cone->materialBindingCorrection)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingPreview, cone->materialBindingPreview)
    PARSE_TYPED_ATTRIBUTE(table, prop, "radius", GeomCone, cone->radius)
    PARSE_TYPED_ATTRIBUTE(table, prop, "height", GeomCone, cone->height)
    PARSE_ENUM_PROPETY(table, prop, "axis", AxisEnumHandler, GeomCone, cone->axis)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, GeomCone,
                       cone->purpose)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", GeomCone, cone->extent)
    ADD_PROPERTY(table, prop, GeomCone, cone->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<GeomCylinder>(
    const PropertyMap &properties,
    const ReferenceList &references,
    GeomCylinder *cylinder,
    std::string *warn,
    std::string *err) {

  (void)warn;
  (void)references;


  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &cylinder->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    DCOUT("prop: " << prop.first);
    PARSE_PROXY_PRIM_RELATION(table, prop, cylinder)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBinding, cylinder->materialBinding)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingCorrection, cylinder->materialBindingCorrection)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingPreview, cylinder->materialBindingPreview)
    PARSE_TYPED_ATTRIBUTE(table, prop, "radius", GeomCylinder,
                         cylinder->radius)
    PARSE_TYPED_ATTRIBUTE(table, prop, "height", GeomCylinder,
                         cylinder->height)
    PARSE_ENUM_PROPETY(table, prop, "axis", AxisEnumHandler, GeomCylinder, cylinder->axis)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, GeomCylinder,
                       cylinder->purpose)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", GeomCylinder, cylinder->extent)
    ADD_PROPERTY(table, prop, GeomCylinder, cylinder->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<GeomCapsule>(
    const PropertyMap &properties,
    const ReferenceList &references,
    GeomCapsule *capsule,
    std::string *warn,
    std::string *err) {

  (void)warn;
  (void)references;

  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &capsule->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    DCOUT("prop: " << prop.first);
    PARSE_PROXY_PRIM_RELATION(table, prop, capsule)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBinding, capsule->materialBinding)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingCorrection, capsule->materialBindingCorrection)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingPreview, capsule->materialBindingPreview)
    PARSE_TYPED_ATTRIBUTE(table, prop, "radius", GeomCapsule, capsule->radius)
    PARSE_TYPED_ATTRIBUTE(table, prop, "height", GeomCapsule, capsule->height)
    PARSE_ENUM_PROPETY(table, prop, "axis", AxisEnumHandler, GeomCapsule, capsule->axis)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, GeomCapsule,
                       capsule->purpose)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", GeomCapsule, capsule->extent)
    ADD_PROPERTY(table, prop, GeomCapsule, capsule->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<GeomCube>(
    const PropertyMap &properties,
    const ReferenceList &references,
    GeomCube *cube,
    std::string *warn,
    std::string *err) {

  (void)warn;
  (void)references;

  //
  // pxrUSD says... "If you author size you must also author extent."
  //
  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &cube->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    DCOUT("prop: " << prop.first);
    PARSE_PROXY_PRIM_RELATION(table, prop, cube)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBinding, cube->materialBinding)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingCorrection, cube->materialBindingCorrection)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingPreview, cube->materialBindingPreview)
    PARSE_TYPED_ATTRIBUTE(table, prop, "size", GeomCube, cube->size)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", GeomCube, cube->extent)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, GeomCube,
                       cube->purpose)
    ADD_PROPERTY(table, prop, GeomCube, cube->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<GeomMesh>(
    const PropertyMap &properties,
    const ReferenceList &references,
    GeomMesh *mesh,
    std::string *warn,
    std::string *err) {

  (void)references;

  DCOUT("GeomMesh");

  auto SubdivisioSchemeHandler = [](const std::string &tok)
      -> nonstd::expected<GeomMesh::SubdivisionScheme, std::string> {
    using EnumTy = std::pair<GeomMesh::SubdivisionScheme, const char *>;
    const std::vector<EnumTy> enums = {
        std::make_pair(GeomMesh::SubdivisionScheme::SubdivisionSchemeNone, "none"),
        std::make_pair(GeomMesh::SubdivisionScheme::CatmullClark,
                       "catmullClark"),
        std::make_pair(GeomMesh::SubdivisionScheme::Loop, "loop"),
        std::make_pair(GeomMesh::SubdivisionScheme::Bilinear, "bilinear"),
    };
    return EnumHandler<GeomMesh::SubdivisionScheme>("subdivisionScheme", tok,
                                                    enums);
  };

  auto InterpolateBoundaryHandler = [](const std::string &tok)
      -> nonstd::expected<GeomMesh::InterpolateBoundary, std::string> {
    using EnumTy = std::pair<GeomMesh::InterpolateBoundary, const char *>;
    const std::vector<EnumTy> enums = {
        std::make_pair(GeomMesh::InterpolateBoundary::InterpolateBoundaryNone, "none"),
        std::make_pair(GeomMesh::InterpolateBoundary::EdgeAndCorner,
                       "edgeAndCorner"),
        std::make_pair(GeomMesh::InterpolateBoundary::EdgeOnly, "edgeOnly"),
    };
    return EnumHandler<GeomMesh::InterpolateBoundary>("interpolateBoundary",
                                                      tok, enums);
  };

  auto FaceVaryingLinearInterpolationHandler = [](const std::string &tok)
      -> nonstd::expected<GeomMesh::FaceVaryingLinearInterpolation,
                          std::string> {
    using EnumTy =
        std::pair<GeomMesh::FaceVaryingLinearInterpolation, const char *>;
    const std::vector<EnumTy> enums = {
        std::make_pair(GeomMesh::FaceVaryingLinearInterpolation::CornersPlus1,
                       "cornersPlus1"),
        std::make_pair(GeomMesh::FaceVaryingLinearInterpolation::CornersPlus2,
                       "cornersPlus2"),
        std::make_pair(GeomMesh::FaceVaryingLinearInterpolation::CornersOnly,
                       "cornersOnly"),
        std::make_pair(GeomMesh::FaceVaryingLinearInterpolation::Boundaries,
                       "boundaries"),
        std::make_pair(GeomMesh::FaceVaryingLinearInterpolation::FaceVaryingLinearInterpolationNone, "none"),
        std::make_pair(GeomMesh::FaceVaryingLinearInterpolation::All, "all"),
    };
    return EnumHandler<GeomMesh::FaceVaryingLinearInterpolation>(
        "facevaryingLinearInterpolation", tok, enums);
  };

  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &mesh->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    DCOUT("GeomMesh prop: " << prop.first);
    PARSE_PROXY_PRIM_RELATION(table, prop, mesh)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBinding, mesh->materialBinding)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingCorrection, mesh->materialBindingCorrection)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kMaterialBindingPreview, mesh->materialBindingPreview)
    PARSE_SINGLE_TARGET_PATH_RELATION(table, prop, kSkelSkeleton, mesh->skeleton)
    PARSE_TARGET_PATHS_RELATION(table, prop, kSkelBlendShapeTargets, mesh->blendShapeTargets)
    PARSE_TYPED_ATTRIBUTE(table, prop, "points", GeomMesh, mesh->points)
    PARSE_TYPED_ATTRIBUTE(table, prop, "normals", GeomMesh, mesh->normals)
    PARSE_TYPED_ATTRIBUTE(table, prop, "faceVertexCounts", GeomMesh,
                         mesh->faceVertexCounts)
    PARSE_TYPED_ATTRIBUTE(table, prop, "faceVertexIndices", GeomMesh,
                         mesh->faceVertexIndices)
    // Subd
    PARSE_TYPED_ATTRIBUTE(table, prop, "cornerIndices", GeomMesh,
                         mesh->cornerIndices)
    PARSE_TYPED_ATTRIBUTE(table, prop, "cornerSharpnesses", GeomMesh,
                         mesh->cornerIndices)
    PARSE_TYPED_ATTRIBUTE(table, prop, "creaseIndices", GeomMesh,
                         mesh->cornerIndices)
    PARSE_TYPED_ATTRIBUTE(table, prop, "creaseLengths", GeomMesh,
                         mesh->cornerIndices)
    PARSE_TYPED_ATTRIBUTE(table, prop, "creaseSharpnesses", GeomMesh,
                         mesh->cornerIndices)
    PARSE_TYPED_ATTRIBUTE(table, prop, "holeIndices", GeomMesh,
                         mesh->cornerIndices)
    //
    PARSE_TYPED_ATTRIBUTE(table, prop, "doubleSided", GeomMesh, mesh->doubleSided)

    PARSE_ENUM_PROPETY(table, prop, "subdivisionScheme",
                       SubdivisioSchemeHandler, GeomMesh,
                       mesh->subdivisionScheme)
    PARSE_ENUM_PROPETY(table, prop, "interpolateBoundary",
                       InterpolateBoundaryHandler, GeomMesh,
                       mesh->interpolateBoundary)
    PARSE_ENUM_PROPETY(table, prop, "facevaryingLinearInterpolation",
                       FaceVaryingLinearInterpolationHandler, GeomMesh,
                       mesh->faceVaryingLinearInterpolation)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, GeomMesh,
                       mesh->purpose)
    PARSE_ENUM_PROPETY(table, prop, "orientation", OrientationEnumHandler, GeomMesh,
                       mesh->orientation)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", GeomMesh, mesh->extent)
    // blendShape names
    PARSE_TYPED_ATTRIBUTE(table, prop, kSkelBlendShapes, GeomMesh, mesh->blendShapes)
    // generic
    ADD_PROPERTY(table, prop, GeomMesh, mesh->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }


  return true;
}


template <>
bool ReconstructPrim<GeomCamera>(
    const PropertyMap &properties,
    const ReferenceList &references,
    GeomCamera *camera,
    std::string *warn,
    std::string *err) {

  (void)references;
  (void)warn;

  auto ProjectionHandler = [](const std::string &tok)
      -> nonstd::expected<GeomCamera::Projection, std::string> {
    using EnumTy = std::pair<GeomCamera::Projection, const char *>;
    constexpr std::array<EnumTy, 2> enums = {
        std::make_pair(GeomCamera::Projection::Perspective, "perspective"),
        std::make_pair(GeomCamera::Projection::Orthographic, "orthographic"),
    };

    auto ret =
        CheckAllowedTokens<GeomCamera::Projection, enums.size()>(enums, tok);
    if (!ret) {
      return nonstd::make_unexpected(ret.error());
    }

    for (auto &item : enums) {
      if (tok == item.second) {
        return item.first;
      }
    }

    // Should never reach here, though.
    return nonstd::make_unexpected(
        quote(tok) + " is invalid token for `projection` propety");
  };

  auto StereoRoleHandler = [](const std::string &tok)
      -> nonstd::expected<GeomCamera::StereoRole, std::string> {
    using EnumTy = std::pair<GeomCamera::StereoRole, const char *>;
    constexpr std::array<EnumTy, 3> enums = {
        std::make_pair(GeomCamera::StereoRole::Mono, "mono"),
        std::make_pair(GeomCamera::StereoRole::Left, "left"),
        std::make_pair(GeomCamera::StereoRole::Right, "right"),
    };

    auto ret =
        CheckAllowedTokens<GeomCamera::StereoRole, enums.size()>(enums, tok);
    if (!ret) {
      return nonstd::make_unexpected(ret.error());
    }

    for (auto &item : enums) {
      if (tok == item.second) {
        return item.first;
      }
    }

    // Should never reach here, though.
    return nonstd::make_unexpected(
        quote(tok) + " is invalid token for `stereoRole` propety");
  };

  std::set<std::string> table;

  if (!prim::ReconstructXformOpsFromProperties(table, properties, &camera->xformOps, err)) {
    return false;
  }

  for (const auto &prop : properties) {
    PARSE_TYPED_ATTRIBUTE(table, prop, "focalLength", GeomCamera, camera->focalLength)
    PARSE_TYPED_ATTRIBUTE(table, prop, "focusDistance", GeomCamera,
                   camera->focusDistance)
    PARSE_TYPED_ATTRIBUTE(table, prop, "exposure", GeomCamera, camera->exposure)
    PARSE_TYPED_ATTRIBUTE(table, prop, "fStop", GeomCamera, camera->fStop)
    PARSE_TYPED_ATTRIBUTE(table, prop, "horizontalAperture", GeomCamera,
                   camera->horizontalAperture)
    PARSE_TYPED_ATTRIBUTE(table, prop, "horizontalApertureOffset", GeomCamera,
                   camera->horizontalApertureOffset)
    PARSE_TYPED_ATTRIBUTE(table, prop, "clippingRange", GeomCamera,
                   camera->clippingRange)
    PARSE_TYPED_ATTRIBUTE(table, prop, "clippingPlanes", GeomCamera,
                   camera->clippingPlanes)
    PARSE_TYPED_ATTRIBUTE(table, prop, "shutter:open", GeomCamera, camera->shutterOpen)
    PARSE_TYPED_ATTRIBUTE(table, prop, "shutter:close", GeomCamera,
                   camera->shutterClose)
    PARSE_ENUM_PROPETY(table, prop, "projection", ProjectionHandler, GeomCamera,
                       camera->projection)
    PARSE_ENUM_PROPETY(table, prop, "stereoRole", StereoRoleHandler, GeomCamera,
                       camera->stereoRole)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, GeomCamera,
                         camera->purpose)
    PARSE_ENUM_PROPETY(table, prop, "orientation", OrientationEnumHandler, GeomCamera,
                       camera->orientation)
    PARSE_EXTENT_ATTRIBUTE(table, prop, "extent", GeomCamera, camera->extent)
    ADD_PROPERTY(table, prop, GeomCamera, camera->props)
    PARSE_PROPERTY_END_MAKE_ERROR(table, prop)
  }

  return true;
}

template <>
bool ReconstructShader<UsdPreviewSurface>(
    const PropertyMap &properties,
    const ReferenceList &references,
    UsdPreviewSurface *surface,
    std::string *warn,
    std::string *err) {
  // TODO: references
  (void)references;

  std::set<std::string> table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>
  for (auto &prop : properties) {
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:diffuseColor", UsdPreviewSurface,
                         surface->diffuseColor)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:emissiveColor", UsdPreviewSurface,
                         surface->emissiveColor)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:roughness", UsdPreviewSurface,
                         surface->roughness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:specularColor", UsdPreviewSurface,
                         surface->specularColor)  // specular workflow
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:metallic", UsdPreviewSurface,
                         surface->metallic)  // non specular workflow
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:clearcoat", UsdPreviewSurface,
                         surface->clearcoat)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:clearcoatRoughness",
                         UsdPreviewSurface, surface->clearcoatRoughness)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:opacity", UsdPreviewSurface,
                         surface->opacity)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:opacityThreshold",
                         UsdPreviewSurface, surface->opacityThreshold)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:ior", UsdPreviewSurface,
                         surface->ior)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:normal", UsdPreviewSurface,
                         surface->normal)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:dispacement", UsdPreviewSurface,
                         surface->displacement)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:occlusion", UsdPreviewSurface,
                         surface->occlusion)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:useSpecularWorkflow",
                         UsdPreviewSurface, surface->useSpecularWorkflow)
    PARSE_SHADER_OUTPUT_PROPERTY(table, prop, "outputs:surface", UsdPreviewSurface,
                   surface->outputsSurface)
    PARSE_SHADER_OUTPUT_PROPERTY(table, prop, "outputs:displacement", UsdPreviewSurface,
                   surface->outputsDisplacement)
    ADD_PROPERTY(table, prop, UsdPreviewSurface, surface->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructShader<UsdUVTexture>(
    const PropertyMap &properties,
    const ReferenceList &references,
    UsdUVTexture *texture,
    std::string *warn,
    std::string *err)
{
  // TODO: references
  (void)references;

  auto SourceColorSpaceHandler = [](const std::string &tok)
      -> nonstd::expected<UsdUVTexture::SourceColorSpace, std::string> {
    using EnumTy = std::pair<UsdUVTexture::SourceColorSpace, const char *>;
    const std::vector<EnumTy> enums = {
        std::make_pair(UsdUVTexture::SourceColorSpace::Auto, "auto"),
        std::make_pair(UsdUVTexture::SourceColorSpace::Raw, "raw"),
        std::make_pair(UsdUVTexture::SourceColorSpace::SRGB, "sRGB"),
    };

    return EnumHandler<UsdUVTexture::SourceColorSpace>(
        "inputs:sourceColorSpace", tok, enums);
  };

  auto WrapHandler = [](const std::string &tok)
      -> nonstd::expected<UsdUVTexture::Wrap, std::string> {
    using EnumTy = std::pair<UsdUVTexture::Wrap, const char *>;
    const std::vector<EnumTy> enums = {
        std::make_pair(UsdUVTexture::Wrap::UseMetadata, "useMetadata"),
        std::make_pair(UsdUVTexture::Wrap::Black, "black"),
        std::make_pair(UsdUVTexture::Wrap::Clamp, "clamp"),
        std::make_pair(UsdUVTexture::Wrap::Repeat, "repeat"),
        std::make_pair(UsdUVTexture::Wrap::Mirror, "mirror"),
    };

    return EnumHandler<UsdUVTexture::Wrap>(
        "inputs:wrap*", tok, enums);
  };

  std::set<std::string> table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>

  for (auto &prop : properties) {
    DCOUT("prop.name = " << prop.first);
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:file", UsdUVTexture, texture->file)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:st", UsdUVTexture,
                          texture->st)
    PARSE_ENUM_PROPETY(table, prop, "inputs:sourceColorSpace",
                       SourceColorSpaceHandler, UsdUVTexture,
                       texture->sourceColorSpace)
    PARSE_ENUM_PROPETY(table, prop, "inputs:wrapS",
                       WrapHandler, UsdUVTexture,
                       texture->wrapS)
    PARSE_ENUM_PROPETY(table, prop, "inputs:wrapT",
                       WrapHandler, UsdUVTexture,
                       texture->wrapT)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:r", UsdUVTexture,
                                  texture->outputsR)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:g", UsdUVTexture,
                                  texture->outputsG)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:b", UsdUVTexture,
                                  texture->outputsB)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:a", UsdUVTexture,
                                  texture->outputsA)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:rgb", UsdUVTexture,
                                  texture->outputsRGB)
    ADD_PROPERTY(table, prop, UsdUVTexture, texture->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  DCOUT("UsdUVTexture reconstructed.");
  return true;
}

template <>
bool ReconstructShader<UsdPrimvarReader_int>(
    const PropertyMap &properties,
    const ReferenceList &references,
    UsdPrimvarReader_int *preader,
    std::string *warn,
    std::string *err)
{
  (void)references;
  std::set<std::string> table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>
  for (auto &prop : properties) {
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:fallback", UsdPrimvarReader_int,
                   preader->fallback)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:varname", UsdPrimvarReader_int,
                   preader->varname)  // `token`
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:result",
                                  UsdPrimvarReader_int, preader->result)
    ADD_PROPERTY(table, prop, UsdPrimvarReader_int, preader->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
  return false;
}

template <>
bool ReconstructShader<UsdPrimvarReader_float>(
    const PropertyMap &properties,
    const ReferenceList &references,
    UsdPrimvarReader_float *preader,
    std::string *warn,
    std::string *err)
{
  (void)references;
  std::set<std::string> table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>
  for (auto &prop : properties) {
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:fallback", UsdPrimvarReader_float,
                   preader->fallback)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:varname", UsdPrimvarReader_float,
                   preader->varname)  // `token`
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:result",
                                  UsdPrimvarReader_float, preader->result)
    ADD_PROPERTY(table, prop, UsdPrimvarReader_float, preader->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
  return false;
}

template <>
bool ReconstructShader<UsdPrimvarReader_float2>(
    const PropertyMap &properties,
    const ReferenceList &references,
    UsdPrimvarReader_float2 *preader,
    std::string *warn,
    std::string *err)
{
  (void)references;
  std::set<std::string> table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>
  for (auto &prop : properties) {
    DCOUT("prop = " << prop.first);
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:varname", UsdPrimvarReader_float2,
                   preader->varname)  // `token`
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:fallback", UsdPrimvarReader_float2,
                   preader->fallback)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:result",
                                  UsdPrimvarReader_float2, preader->result)
    ADD_PROPERTY(table, prop, UsdPrimvarReader_float2, preader->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructShader<UsdPrimvarReader_float3>(
    const PropertyMap &properties,
    const ReferenceList &references,
    UsdPrimvarReader_float3 *preader,
    std::string *warn,
    std::string *err)
{
  (void)references;
  std::set<std::string> table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>
  for (auto &prop : properties) {
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:fallback", UsdPrimvarReader_float3,
                   preader->fallback)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:varname", UsdPrimvarReader_float3,
                   preader->varname)  // `token`
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:result",
                                  UsdPrimvarReader_float3, preader->result)
    ADD_PROPERTY(table, prop, UsdPrimvarReader_float3, preader->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructShader<UsdPrimvarReader_float4>(
    const PropertyMap &properties,
    const ReferenceList &references,
    UsdPrimvarReader_float4 *preader,
    std::string *warn,
    std::string *err)
{
  (void)references;
  std::set<std::string> table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>

  for (auto &prop : properties) {
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:fallback", UsdPrimvarReader_float4,
                   preader->fallback)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:varname", UsdPrimvarReader_float4,
                   preader->varname)  // `token`
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:result",
                                  UsdPrimvarReader_float4, preader->result)
    ADD_PROPERTY(table, prop, UsdPrimvarReader_float4, preader->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
  return true;
}

template <>
bool ReconstructShader<UsdTransform2d>(
    const PropertyMap &properties,
    const ReferenceList &references,
    UsdTransform2d *transform,
    std::string *warn,
    std::string *err)
{
  (void)references;
  std::set<std::string> table;
  table.insert("info:id"); // `info:id` is already parsed in ReconstructPrim<Shader>
  for (auto &prop : properties) {
    DCOUT("prop = " << prop.first);
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:in", UsdTransform2d,
                   transform->in)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:rotation", UsdTransform2d,
                   transform->rotation)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:scale", UsdTransform2d,
                   transform->scale)
    PARSE_TYPED_ATTRIBUTE(table, prop, "inputs:translation", UsdTransform2d,
                   transform->translation)
    PARSE_SHADER_TERMINAL_ATTRIBUTE(table, prop, "outputs:result",
                                  UsdTransform2d, transform->result)
    ADD_PROPERTY(table, prop, UsdPrimvarReader_float2, transform->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }

  return true;
}

template <>
bool ReconstructPrim<Shader>(
    const PropertyMap &properties,
    const ReferenceList &references,
    Shader *shader,
    std::string *warn,
    std::string *err)
{
  (void)properties;

  constexpr auto kUsdPreviewSurface = "UsdPreviewSurface";
  constexpr auto kUsdUVTexture = "UsdUVTexture";
  constexpr auto kUsdPrimvarReader_int = "UsdPrimvarReader_int";
  constexpr auto kUsdPrimvarReader_float = "UsdPrimvarReader_float";
  constexpr auto kUsdPrimvarReader_float2 = "UsdPrimvarReader_float2";
  constexpr auto kUsdPrimvarReader_float3 = "UsdPrimvarReader_float3";
  constexpr auto kUsdPrimvarReader_float4 = "UsdPrimvarReader_float4";
  constexpr auto kUsdTransform2d = "UsdTransform2d";

  auto info_id_prop = properties.find("info:id");
  if (info_id_prop == properties.end()) {
    // Generic? Shader. Currently report as an error.
    PUSH_ERROR_AND_RETURN("`Shader` must contain `info:id` property.");
  }

  std::string shader_type;
  if (info_id_prop->second.is_attribute()) {
    const Attribute &attr = info_id_prop->second.get_attribute();
    if ((attr.type_name() == value::kToken)) {
      if (auto pv = attr.get_value<value::token>()) {
        shader_type = pv.value().str();
      } else {
        PUSH_ERROR_AND_RETURN("Internal errror. `info:id` has invalid type.");
      }
    } else {
      PUSH_ERROR_AND_RETURN("`info:id` attribute must be `token` type.");
    }

    // For some corrupted? USDZ file does not have `uniform` variability.
    if (attr.variability() != Variability::Uniform) {
      PUSH_WARN("`info:id` attribute must have `uniform` variability.");
    }
  } else {
    PUSH_ERROR_AND_RETURN("Invalid type or value for `info:id` property in `Shader`.");
  }


  DCOUT("info:id = " << shader_type);

  if (shader_type.compare(kUsdPreviewSurface) == 0) {
    UsdPreviewSurface surface;
    if (!ReconstructShader<UsdPreviewSurface>(properties, references,
                                              &surface, warn, err)) {
      PUSH_ERROR_AND_RETURN("Failed to Reconstruct " << kUsdPreviewSurface);
    }
    shader->info_id = kUsdPreviewSurface;
    shader->value = surface;
    DCOUT("info_id = " << shader->info_id);
  } else if (shader_type.compare(kUsdUVTexture) == 0) {
    UsdUVTexture texture;
    if (!ReconstructShader<UsdUVTexture>(properties, references,
                                         &texture, warn, err)) {
      PUSH_ERROR_AND_RETURN("Failed to Reconstruct " << kUsdUVTexture);
    }
    shader->info_id = kUsdUVTexture;
    shader->value = texture;
  } else if (shader_type.compare(kUsdPrimvarReader_int) == 0) {
    UsdPrimvarReader_int preader;
    if (!ReconstructShader<UsdPrimvarReader_int>(properties, references,
                                                 &preader, warn, err)) {
      PUSH_ERROR_AND_RETURN("Failed to Reconstruct "
                            << kUsdPrimvarReader_int);
    }
    shader->info_id = kUsdPrimvarReader_int;
    shader->value = preader;
  } else if (shader_type.compare(kUsdPrimvarReader_float) == 0) {
    UsdPrimvarReader_float preader;
    if (!ReconstructShader<UsdPrimvarReader_float>(properties, references,
                                                   &preader, warn, err)) {
      PUSH_ERROR_AND_RETURN("Failed to Reconstruct "
                            << kUsdPrimvarReader_float);
    }
    shader->info_id = kUsdPrimvarReader_float;
    shader->value = preader;
  } else if (shader_type.compare(kUsdPrimvarReader_float2) == 0) {
    UsdPrimvarReader_float2 preader;
    if (!ReconstructShader<UsdPrimvarReader_float2>(properties, references,
                                                    &preader, warn, err)) {
      PUSH_ERROR_AND_RETURN("Failed to Reconstruct "
                            << kUsdPrimvarReader_float2);
    }
    shader->info_id = kUsdPrimvarReader_float2;
    shader->value = preader;
  } else if (shader_type.compare(kUsdPrimvarReader_float3) == 0) {
    UsdPrimvarReader_float3 preader;
    if (!ReconstructShader<UsdPrimvarReader_float3>(properties, references,
                                                    &preader, warn, err)) {
      PUSH_ERROR_AND_RETURN("Failed to Reconstruct "
                            << kUsdPrimvarReader_float3);
    }
    shader->info_id = kUsdPrimvarReader_float3;
    shader->value = preader;
  } else if (shader_type.compare(kUsdPrimvarReader_float4) == 0) {
    UsdPrimvarReader_float4 preader;
    if (!ReconstructShader<UsdPrimvarReader_float4>(properties, references,
                                                    &preader, warn, err)) {
      PUSH_ERROR_AND_RETURN("Failed to Reconstruct "
                            << kUsdPrimvarReader_float4);
    }
    shader->info_id = kUsdPrimvarReader_float4;
    shader->value = preader;
  } else if (shader_type.compare(kUsdTransform2d) == 0) {
    UsdTransform2d transform;
    if (!ReconstructShader<UsdTransform2d>(properties, references,
                                                    &transform, warn, err)) {
      PUSH_ERROR_AND_RETURN("Failed to Reconstruct "
                            << kUsdTransform2d);
    }
    shader->info_id = kUsdTransform2d;
    shader->value = transform;
  } else {
    // TODO: string, point, vector, matrix
    PUSH_ERROR_AND_RETURN(
        "Invalid or Unsupported Shader type. info:id = \"" + shader_type +
        "\n");
  }

  DCOUT("Shader reconstructed.");

  return true;
}

template <>
bool ReconstructPrim<Material>(
    const PropertyMap &properties,
    const ReferenceList &references,
    Material *material,
    std::string *warn,
    std::string *err)
{
  (void)references;
  std::set<std::string> table;

  // TODO: special treatment for properties with 'inputs' and 'outputs' namespace.

  // For `Material`, `outputs` are terminal attribute and treated as input attribute with connection(Should be "token output:surface.connect = </path/to/shader>").
  for (auto &prop : properties) {
    PARSE_SHADER_INPUT_CONNECTION_PROPERTY(table, prop, "outputs:surface",
                                  Material, material->surface)
    PARSE_SHADER_INPUT_CONNECTION_PROPERTY(table, prop, "outputs:volume",
                                  Material, material->volume)
    PARSE_ENUM_PROPETY(table, prop, "purpose", PurposeEnumHandler, Material,
                       material->purpose)
    ADD_PROPERTY(table, prop, Material, material->props)
    PARSE_PROPERTY_END_MAKE_WARN(table, prop)
  }
  return true;
}


} // namespace prim
} // namespace tinyusdz
