// SPDX-License-Identifier: MIT
// Copyright 2022 - Present, Syoyo Fujita.
//
// UsdGeom
#pragma once

#include "prim-types.hh"
#include "value-types.hh"
#include "xform.hh"

namespace tinyusdz {

// From schema definition.
constexpr auto kGPrim = "GPrim";
constexpr auto kGeomCube = "Cube";
constexpr auto kGeomXform = "Xform";
constexpr auto kGeomMesh = "Mesh";
constexpr auto kGeomSubset = "GeomSubset";
constexpr auto kGeomBasisCurves = "BasisCurves";
constexpr auto kGeomCylinder = "Cylinder";
constexpr auto kGeomCapsule = "Capsule";
constexpr auto kGeomPoints = "Points";
constexpr auto kGeomCone = "Cone";
constexpr auto kGeomSphere = "Sphere";
constexpr auto kGeomCamera = "Camera";
constexpr auto kPointInstancer = "PointInstancer";

struct GPrim;

bool IsSupportedGeomPrimvarType(uint32_t tyid);
bool IsSupportedGeomPrimvarType(const std::string &type_name);
  
//
// GeomPrimvar is a wrapper class for Attribute and indices(for Indexed Primvar)
// - Attribute with `primvars` prefix. e.g. "primvars:
// - Optional: indices.
//
// Currently this class COPIES variable from GPrim.
// GeomPrimvar is only constructable from GPrim.
//
// Currently read-only operation are provided and TinyUSDZ does not recommend to use GeomPrimar to construct Prim's property at the moment.
// (Please operate on `props` directly)
//
//
class GeomPrimvar {

 friend GPrim;

 public:
  GeomPrimvar() = default;

  //GeomPrimvar(const Attribute &attr) : _attr(attr) {}
  //GeomPrimvar(const Attribute &attr, const std::vector<int32_t> &indices) : _attr(attr), _indices(indices) {}

  ///
  /// For Indexed Primvar(array value + indices)
  ///
  /// equivalent to ComputeFlattened in pxrUSD.
  ///
  /// ```
  /// for i in len(indices):
  ///   dest[i] = values[indices[i]]
  /// ```
  ///
  /// If Primvar does not have indices, return attribute value as is(same with `get_value`).
  ///
  /// Return false when operation failed or if the attribute type is not supported for Indexed Primvar.
  ///
  template <typename T>
  bool flatten_with_indices(T *dst, std::string *err = nullptr);

  template <typename T>
  bool flatten_with_indices(std::vector<T> *dst, std::string *err = nullptr);

  // Generic Value version.
  bool flatten_with_indices(value::Value *dst, std::string *err = nullptr);

  bool has_elementSize() const;
  uint32_t get_elementSize() const;

  bool has_interpolation() const;
  Interpolation get_interpolation() const;

  const std::vector<int32_t> &get_indices() { return _indices; }

  bool has_indices() { return _indices.size(); }

  uint32_t type_id() { return _attr.type_id(); }
  std::string type_name() { return _attr.type_name(); }

  // Name of Primvar. "primvars:" prefix(namespace) is omitted.
  const std::string name() const { return _name; }

  ///
  /// Attribute has value?(Not empty)
  ///
  bool has_value() const;

 private:
  void set_name(const std::string &name) { _name = name; }
  void set_attribute(const Attribute &attr) {
    _attr = attr;
  }

  void set_indices(const std::vector<int32_t> &indices) {
    _indices = indices;
  }

  std::string _name;
  Attribute _attr;
  std::vector<int32_t> _indices;  // TODO: uint support?

  bool get_value(const value::Value *value,
                 const double t = value::TimeCode::Default(),
                 const value::TimeSampleInterpolationType tinterp =
                     value::TimeSampleInterpolationType::Held);

};

// Geometric Prim. Encapsulates Imagable + Boundable in pxrUSD schema.
// <pxrUSD>/pxr/usd/usdGeom/schema.udsa

struct GPrim : Xformable {
  std::string name;
  Specifier spec{Specifier::Def};

  int64_t parent_id{-1};  // Index to parent node

  std::string prim_type;  // Primitive type(if specified by `def`)

  // Gprim

  TypedAttribute<Animatable<Extent>>
      extent;  // bounding extent. When authorized, the extent is the bounding
               // box of whole its children.

  TypedAttributeWithFallback<bool> doubleSided{
      false};  // "uniform bool doubleSided"

  TypedAttributeWithFallback<Orientation> orientation{
      Orientation::RightHanded};  // "uniform token orientation"
  TypedAttributeWithFallback<Animatable<Visibility>> visibility{
      Visibility::Inherited};  // "token visibility"
  TypedAttributeWithFallback<Purpose> purpose{
      Purpose::Default};  // "uniform token purpose"

#if 0  // TODO: Remove. Please use `props["primvars:displayColor"]`
  TypedAttribute<Animatable<value::color3f>>
      displayColor;                                    // primvars:displayColor
  TypedAttribute<Animatable<float>> displayOpacity;  // primvars:displaOpacity
#endif

  nonstd::optional<Relationship> proxyPrim;
  nonstd::optional<MaterialBindingAPI> materialBinding;

  std::map<std::string, Property> props;

  std::map<std::string, VariantSet> variantSet;

  bool _valid{true};  // default behavior is valid(allow empty GPrim)

  // Prim metadataum.
  PrimMeta meta;

  // For GeomPrimvar.

  ///
  /// Get Attribute(+ indices Attribute for Indexed Primvar) with "primvars:" suffix(namespace) in `props`
  ///
  /// @param[in] name Primvar name(`primvars:` prefix omitted. e.g. "normals", "st0", ...)
  /// @param[out] primvar GeomPrimvar output.
  /// @param[out] err Optional Error message(filled when returning false)
  ///
  bool get_primvar(const std::string &name, GeomPrimvar *primvar, std::string *err = nullptr) const;

  ///
  /// Check if primvar exists with given name
  ///
  /// @param[in] name Primvar name(`primvars:` prefix omitted. e.g. "normals", "st0", ...)
  ///
  bool has_primvar(const std::string &name) const;
  
  ///
  /// Return List of Primvar in this GPrim contains.
  ///
  std::vector<GeomPrimvar> get_primvars() const;

};

struct Xform : GPrim {
  // Xform() {}
};

// GeomSubset
struct GeomSubset {
  enum class ElementType { Face };

  enum class FamilyType {
    Partition,       // 'partition'
    NonOverlapping,  // 'nonOverlapping'
    Unrestricted,    // 'unrestricted' (fallback)
  };

  std::string name;
  Specifier spec;

  int64_t parent_id{-1};  // Index to parent node

  ElementType elementType{ElementType::Face};  // must be face
  FamilyType familyType{FamilyType::Unrestricted};
  nonstd::optional<value::token> familyName;  // "token familyName"

  nonstd::expected<bool, std::string> SetElementType(const std::string &str) {
    if (str == "face") {
      elementType = ElementType::Face;
      return true;
    }

    return nonstd::make_unexpected(
        "Only `face` is supported for `elementType`, but `" + str +
        "` specified");
  }

  nonstd::expected<bool, std::string> SetFamilyType(const std::string &str) {
    if (str == "partition") {
      familyType = FamilyType::Partition;
      return true;
    } else if (str == "nonOverlapping") {
      familyType = FamilyType::NonOverlapping;
      return true;
    } else if (str == "unrestricted") {
      familyType = FamilyType::Unrestricted;
      return true;
    }

    return nonstd::make_unexpected("Invalid `familyType` specified: `" + str +
                                   "`.");
  }

  std::vector<uint32_t> indices;

  std::map<std::string, Property> props;  // custom Properties
  PrimMeta meta;
};

// Polygon mesh geometry
// X11's X.h uses `None` macro, so add extra prefix to `None` enum
struct GeomMesh : GPrim {
  enum class InterpolateBoundary {
    InterpolateBoundaryNone,  // "none"
    EdgeAndCorner,            // "edgeAndCorner"
    EdgeOnly                  // "edgeOnly"
  };

  enum class FaceVaryingLinearInterpolation {
    CornersPlus1,                        // "cornersPlus1"
    CornersPlus2,                        // "cornersPlus2"
    CornersOnly,                         // "cornersOnly"
    Boundaries,                          // "boundaries"
    FaceVaryingLinearInterpolationNone,  // "none"
    All,                                 // "all"
  };

  enum class SubdivisionScheme {
    CatmullClark,           // "catmullClark"
    Loop,                   // "loop"
    Bilinear,               // "bilinear"
    SubdivisionSchemeNone,  // "none"
  };

  //
  // Predefined attribs.
  //
  TypedAttribute<Animatable<std::vector<value::point3f>>> points;  // point3f[]
  TypedAttribute<Animatable<std::vector<value::normal3f>>>
      normals;  // normal3f[] (NOTE: "primvars:normals" are stored in
                // `GPrim::props`)

  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      velocities;  // vector3f[]

  TypedAttribute<Animatable<std::vector<int32_t>>>
      faceVertexCounts;  // int[] faceVertexCounts
  TypedAttribute<Animatable<std::vector<int32_t>>>
      faceVertexIndices;  // int[] faceVertexIndices

  // Make SkelBindingAPI first citizen.
  nonstd::optional<Path> skeleton;  // rel skel:skeleton

  //
  // Utility functions
  //

  // Initialize GeomMesh by GPrim(prepend references)
  void Initialize(const GPrim &pprim);

  // Update GeomMesh by GPrim(append references)
  void UpdateBy(const GPrim &pprim);

  ///
  /// @brief Returns `points`.
  ///
  /// @param[in] time Time for TimeSampled `points` data.
  /// @param[in] interp Interpolation type for TimeSampled `points` data
  /// @return points vector(copied). Returns empty when `points` attribute is
  /// not defined.
  ///
  const std::vector<value::point3f> GetPoints(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;

  ///
  /// @brief Returns normals vector. Precedence order: `primvars:normals` then
  /// `normals`.
  ///
  /// @return normals vector(copied). Returns empty normals vector when neither
  /// `primvars:normals` nor `normals` attribute defined, attribute is a
  /// relation or normals attribute have invalid type(other than `normal3f`).
  ///
  const std::vector<value::normal3f> GetNormals(
      double time = value::TimeCode::Default(),
      value::TimeSampleInterpolationType interp =
          value::TimeSampleInterpolationType::Linear) const;

  ///
  /// @brief Get interpolation of `primvars:normals`, then `normals`.
  /// @return Interpolation of normals. `vertex` by defaut.
  ///
  Interpolation GetNormalsInterpolation() const;

  ///
  /// @brief Returns `faceVertexCounts`.
  ///
  /// @return face vertex counts vector(copied)
  ///
  const std::vector<int32_t> GetFaceVertexCounts() const;

  ///
  /// @brief Returns `faceVertexIndices`.
  ///
  /// @return face vertex indices vector(copied)
  ///
  const std::vector<int32_t> GetFaceVertexIndices() const;

  //
  // SubD attribs.
  //
  TypedAttribute<Animatable<std::vector<int32_t>>>
      cornerIndices;  // int[] cornerIndices
  TypedAttribute<Animatable<std::vector<float>>>
      cornerSharpnesses;  // float[] cornerSharpnesses
  TypedAttribute<Animatable<std::vector<int32_t>>>
      creaseIndices;  // int[] creaseIndices
  TypedAttribute<Animatable<std::vector<int32_t>>>
      creaseLengths;  // int[] creaseLengths
  TypedAttribute<Animatable<std::vector<float>>>
      creaseSharpnesses;  // float[] creaseSharpnesses
  TypedAttribute<Animatable<std::vector<int32_t>>>
      holeIndices;  // int[] holeIndices
  TypedAttributeWithFallback<Animatable<InterpolateBoundary>>
      interpolateBoundary{
          InterpolateBoundary::EdgeAndCorner};  // token interpolateBoundary
  TypedAttributeWithFallback<SubdivisionScheme> subdivisionScheme{
      SubdivisionScheme::CatmullClark};  // uniform token subdivisionScheme
  TypedAttributeWithFallback<Animatable<FaceVaryingLinearInterpolation>>
      faceVaryingLinearInterpolation{
          FaceVaryingLinearInterpolation::
              CornersPlus1};  // token faceVaryingLinearInterpolation

  //
  // TODO: Make SkelBindingAPI property first citizen
  // - int[] primvars:skel:jointIndices
  // - float[] primvars:skel:jointWeights
  // - uniform token[] skel:blendShapes
  // - uniform token[] skel:blendShapeTargets

  //
  // GeomSubset
  //
  // uniform token `subsetFamily:materialBind:familyType`
  GeomSubset::FamilyType materialBindFamilyType{
      GeomSubset::FamilyType::Partition};

  std::vector<GeomSubset> geom_subset_children;

  ///
  /// Validate GeomSubset data whose are attached to this GeomMesh.
  ///
  nonstd::expected<bool, std::string> ValidateGeomSubset();
};

struct GeomCamera : public GPrim {
  enum class Projection {
    Perspective,   // "perspective"
    Orthographic,  // "orthographic"
  };

  enum class StereoRole {
    Mono,   // "mono"
    Left,   // "left"
    Right,  // "right"
  };

  //
  // Properties
  //

  TypedAttribute<std::vector<value::float4>> clippingPlanes;
  TypedAttributeWithFallback<Animatable<value::float2>> clippingRange{
      value::float2({0.1f, 1000000.0f})};
  TypedAttributeWithFallback<Animatable<float>> exposure{0.0f};  // in EV
  TypedAttributeWithFallback<Animatable<float>> focalLength{50.0f};
  TypedAttributeWithFallback<Animatable<float>> focusDistance{0.0f};
  TypedAttributeWithFallback<Animatable<float>> horizontalAperture{20.965f};
  TypedAttributeWithFallback<Animatable<float>> horizontalApertureOffset{0.0f};
  TypedAttributeWithFallback<Animatable<float>> verticalAperture{15.2908f};
  TypedAttributeWithFallback<Animatable<float>> verticalApertureOffset{0.0f};
  TypedAttributeWithFallback<Animatable<float>> fStop{
      0.0f};  // 0.0 = no focusing
  TypedAttributeWithFallback<Animatable<Projection>> projection{
      Projection::Perspective};  // "token projection" Animatable

  TypedAttributeWithFallback<StereoRole> stereoRole{
      StereoRole::Mono};  // "uniform token stereoRole"

  TypedAttributeWithFallback<Animatable<double>> shutterClose{
      0.0};  // double shutter:close
  TypedAttributeWithFallback<Animatable<double>> shutterOpen{
      0.0};  // double shutter:open
};

// struct GeomBoundable : GPrim {};

struct GeomCone : public GPrim {
  //
  // Properties
  //
  TypedAttributeWithFallback<Animatable<double>> height{2.0};
  TypedAttributeWithFallback<Animatable<double>> radius{1.0};

  nonstd::optional<Axis> axis;  // uniform token axis
};

struct GeomCapsule : public GPrim {
  //
  // Properties
  //
  TypedAttributeWithFallback<Animatable<double>> height{2.0};
  TypedAttributeWithFallback<Animatable<double>> radius{0.5};
  nonstd::optional<Axis> axis;  // uniform token axis
};

struct GeomCylinder : public GPrim {
  //
  // Properties
  //
  TypedAttributeWithFallback<Animatable<double>> height{2.0};
  TypedAttributeWithFallback<Animatable<double>> radius{1.0};
  nonstd::optional<Axis> axis;  // uniform token axis
};

struct GeomCube : public GPrim {
  //
  // Properties
  //
  TypedAttributeWithFallback<Animatable<double>> size{2.0};
};

struct GeomSphere : public GPrim {
  //
  // Predefined attribs.
  //
  TypedAttributeWithFallback<Animatable<double>> radius{2.0};
};

//
// Basis Curves(for hair/fur)
//
struct GeomBasisCurves : public GPrim {
  enum class Type {
    Cubic,   // "cubic"(default)
    Linear,  // "linear"
  };

  enum class Basis {
    Bezier,      // "bezier"(default)
    Bspline,     // "bspline"
    CatmullRom,  // "catmullRom"
  };

  enum class Wrap {
    Nonperiodic,  // "nonperiodic"(default)
    Periodic,     // "periodic"
    Pinned,       // "pinned"
  };

  nonstd::optional<Type> type;
  nonstd::optional<Basis> basis;
  nonstd::optional<Wrap> wrap;

  //
  // Predefined attribs.
  //
  TypedAttribute<Animatable<std::vector<value::point3f>>> points;    // point3f
  TypedAttribute<Animatable<std::vector<value::normal3f>>> normals;  // normal3f
  TypedAttribute<Animatable<std::vector<int>>> curveVertexCounts;
  TypedAttribute<Animatable<std::vector<float>>> widths;
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      velocities;  // vector3f
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      accelerations;  // vector3f
};

//
// Points primitive.
//
struct GeomPoints : public GPrim {
  //
  // Predefined attribs.
  //
  TypedAttribute<Animatable<std::vector<value::point3f>>> points;  // point3f[]
  TypedAttribute<Animatable<std::vector<value::normal3f>>>
      normals;                                            // normal3f[]
  TypedAttribute<Animatable<std::vector<float>>> widths;  // float[]
  TypedAttribute<Animatable<std::vector<int64_t>>>
      ids;  // int64[] per-point ids.
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      velocities;  // vector3f[]
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      accelerations;  // vector3f[]
};

//
// Point instancer(TODO).
//
struct PointInstancer : public GPrim {
  nonstd::optional<Relationship> prototypes;  // rel prototypes

  TypedAttribute<Animatable<std::vector<int32_t>>>
      protoIndices;                                      // int[] protoIndices
  TypedAttribute<Animatable<std::vector<int64_t>>> ids;  // int64[] protoIndices
  TypedAttribute<Animatable<std::vector<value::point3f>>>
      positions;  // point3f[] positions
  TypedAttribute<Animatable<std::vector<value::quath>>>
      orientations;  // quath[] orientations
  TypedAttribute<Animatable<std::vector<value::float3>>>
      scales;  // float3[] scales
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      velocities;  // vector3f[] velocities
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      accelerations;  // vector3f[] accelerations
  TypedAttribute<Animatable<std::vector<value::vector3f>>>
      angularVelocities;  // vector3f[] angularVelocities
  TypedAttribute<Animatable<std::vector<int64_t>>>
      invisibleIds;  // int64[] invisibleIds
};


// import DEFINE_TYPE_TRAIT and DEFINE_ROLE_TYPE_TRAIT
#include "define-type-trait.inc"

namespace value {

// Geom
DEFINE_TYPE_TRAIT(GPrim, kGPrim, TYPE_ID_GPRIM, 1);

DEFINE_TYPE_TRAIT(Xform, kGeomXform, TYPE_ID_GEOM_XFORM, 1);
DEFINE_TYPE_TRAIT(GeomMesh, kGeomMesh, TYPE_ID_GEOM_MESH, 1);
DEFINE_TYPE_TRAIT(GeomBasisCurves, kGeomBasisCurves, TYPE_ID_GEOM_BASIS_CURVES,
                  1);
DEFINE_TYPE_TRAIT(GeomSphere, kGeomSphere, TYPE_ID_GEOM_SPHERE, 1);
DEFINE_TYPE_TRAIT(GeomCube, kGeomCube, TYPE_ID_GEOM_CUBE, 1);
DEFINE_TYPE_TRAIT(GeomCone, kGeomCone, TYPE_ID_GEOM_CONE, 1);
DEFINE_TYPE_TRAIT(GeomCylinder, kGeomCylinder, TYPE_ID_GEOM_CYLINDER, 1);
DEFINE_TYPE_TRAIT(GeomCapsule, kGeomCapsule, TYPE_ID_GEOM_CAPSULE, 1);
DEFINE_TYPE_TRAIT(GeomPoints, kGeomPoints, TYPE_ID_GEOM_POINTS, 1);
DEFINE_TYPE_TRAIT(GeomSubset, kGeomSubset, TYPE_ID_GEOM_GEOMSUBSET, 1);
DEFINE_TYPE_TRAIT(GeomCamera, kGeomCamera, TYPE_ID_GEOM_CAMERA, 1);
DEFINE_TYPE_TRAIT(PointInstancer, kPointInstancer, TYPE_ID_GEOM_POINT_INSTANCER,
                  1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

}  // namespace tinyusdz
