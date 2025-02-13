// Apply __FUNC to value types(but no string types).
#define APPLY_FUNC_TO_VALUE_TYPES_NO_STRING(__FUNC) \
  __FUNC(bool)                            \
  __FUNC(value::AssetPath)                \
  __FUNC(value::token)                    \
  __FUNC(value::half)                     \
  __FUNC(value::half2)                    \
  __FUNC(value::half3)                    \
  __FUNC(value::half4)                    \
  __FUNC(int32_t)                         \
  __FUNC(uint32_t)                        \
  __FUNC(value::int2)                     \
  __FUNC(value::int3)                     \
  __FUNC(value::int4)                     \
  __FUNC(value::uint2)                    \
  __FUNC(value::uint3)                    \
  __FUNC(value::uint4)                    \
  __FUNC(int64_t)                         \
  __FUNC(uint64_t)                        \
  __FUNC(float)                           \
  __FUNC(value::float2)                   \
  __FUNC(value::float3)                   \
  __FUNC(value::float4)                   \
  __FUNC(double)                          \
  __FUNC(value::double2)                  \
  __FUNC(value::double3)                  \
  __FUNC(value::double4)                  \
  __FUNC(value::quath)                    \
  __FUNC(value::quatf)                    \
  __FUNC(value::quatd)                    \
  __FUNC(value::normal3h)                 \
  __FUNC(value::normal3f)                 \
  __FUNC(value::normal3d)                 \
  __FUNC(value::vector3h)                 \
  __FUNC(value::vector3f)                 \
  __FUNC(value::vector3d)                 \
  __FUNC(value::point3h)                  \
  __FUNC(value::point3f)                  \
  __FUNC(value::point3d)                  \
  __FUNC(value::color3f)                  \
  __FUNC(value::color3d)                  \
  __FUNC(value::color4h)                  \
  __FUNC(value::color4f)                  \
  __FUNC(value::color4d)                  \
  __FUNC(value::texcoord2h)               \
  __FUNC(value::texcoord2f)               \
  __FUNC(value::texcoord2d)               \
  __FUNC(value::texcoord3h)               \
  __FUNC(value::texcoord3f)               \
  __FUNC(value::texcoord3d)               \
  __FUNC(value::matrix2d)                 \
  __FUNC(value::matrix3d)                 \
  __FUNC(value::matrix4d)                 \
  __FUNC(value::frame4d) \
  __FUNC(std::vector<bool>)                            \
  __FUNC(std::vector<value::AssetPath>)                \
  __FUNC(std::vector<value::token>)                    \
  __FUNC(std::vector<std::string>)                     \
  __FUNC(std::vector<value::StringData>)               \
  __FUNC(std::vector<value::half>)                     \
  __FUNC(std::vector<value::half2>)                    \
  __FUNC(std::vector<value::half3>)                    \
  __FUNC(std::vector<value::half4>)                    \
  __FUNC(std::vector<int32_t>)                         \
  __FUNC(std::vector<uint32_t>)                        \
  __FUNC(std::vector<value::int2>)                     \
  __FUNC(std::vector<value::int3>)                     \
  __FUNC(std::vector<value::int4>)                     \
  __FUNC(std::vector<value::uint2>)                    \
  __FUNC(std::vector<value::uint3>)                    \
  __FUNC(std::vector<value::uint4>)                    \
  __FUNC(std::vector<int64_t>)                         \
  __FUNC(std::vector<uint64_t>)                        \
  __FUNC(std::vector<float>)                           \
  __FUNC(std::vector<value::float2>)                   \
  __FUNC(std::vector<value::float3>)                   \
  __FUNC(std::vector<value::float4>)                   \
  __FUNC(std::vector<double>)                          \
  __FUNC(std::vector<value::double2>)                  \
  __FUNC(std::vector<value::double3>)                  \
  __FUNC(std::vector<value::double4>)                  \
  __FUNC(std::vector<value::quath>)                    \
  __FUNC(std::vector<value::quatf>)                    \
  __FUNC(std::vector<value::quatd>)                    \
  __FUNC(std::vector<value::normal3h>)                 \
  __FUNC(std::vector<value::normal3f>)                 \
  __FUNC(std::vector<value::normal3d>)                 \
  __FUNC(std::vector<value::vector3h>)                 \
  __FUNC(std::vector<value::vector3f>)                 \
  __FUNC(std::vector<value::vector3d>)                 \
  __FUNC(std::vector<value::point3h>)                  \
  __FUNC(std::vector<value::point3f>)                  \
  __FUNC(std::vector<value::point3d>)                  \
  __FUNC(std::vector<value::color3f>)                  \
  __FUNC(std::vector<value::color3d>)                  \
  __FUNC(std::vector<value::color4h>)                  \
  __FUNC(std::vector<value::color4f>)                  \
  __FUNC(std::vector<value::color4d>)                  \
  __FUNC(std::vector<value::texcoord2h>)               \
  __FUNC(std::vector<value::texcoord2f>)               \
  __FUNC(std::vector<value::texcoord2d>)               \
  __FUNC(std::vector<value::texcoord3h>)               \
  __FUNC(std::vector<value::texcoord3f>)               \
  __FUNC(std::vector<value::texcoord3d>)               \
  __FUNC(std::vector<value::matrix2d>)                 \
  __FUNC(std::vector<value::matrix3d>)                 \
  __FUNC(std::vector<value::matrix4d>)                 \
  __FUNC(std::vector<value::frame4d>) \

// Apply __FUNC to value types.
#define APPLY_FUNC_TO_VALUE_TYPES(__FUNC) \
  APPLY_FUNC_TO_VALUE_TYPES_NO_STRING(__FUNC) \
  __FUNC(std::string)                     \
  __FUNC(value::StringData)               \

// Apply __FUNC to numeric value types.
#define APPLY_FUNC_TO_NUMERIC_VALUE_TYPES(__FUNC) \
  __FUNC(bool)                                    \
  __FUNC(value::half)                             \
  __FUNC(value::half2)                            \
  __FUNC(value::half3)                            \
  __FUNC(value::half4)                            \
  __FUNC(int32_t)                                 \
  __FUNC(uint32_t)                                \
  __FUNC(value::int2)                             \
  __FUNC(value::int3)                             \
  __FUNC(value::int4)                             \
  __FUNC(value::uint2)                            \
  __FUNC(value::uint3)                            \
  __FUNC(value::uint4)                            \
  __FUNC(int64_t)                                 \
  __FUNC(uint64_t)                                \
  __FUNC(float)                                   \
  __FUNC(value::float2)                           \
  __FUNC(value::float3)                           \
  __FUNC(value::float4)                           \
  __FUNC(double)                                  \
  __FUNC(value::double2)                          \
  __FUNC(value::double3)                          \
  __FUNC(value::double4)                          \
  __FUNC(value::quath)                            \
  __FUNC(value::quatf)                            \
  __FUNC(value::quatd)                            \
  __FUNC(value::normal3h)                         \
  __FUNC(value::normal3f)                         \
  __FUNC(value::normal3d)                         \
  __FUNC(value::vector3h)                         \
  __FUNC(value::vector3f)                         \
  __FUNC(value::vector3d)                         \
  __FUNC(value::point3h)                          \
  __FUNC(value::point3f)                          \
  __FUNC(value::point3d)                          \
  __FUNC(value::color3f)                          \
  __FUNC(value::color3d)                          \
  __FUNC(value::color4h)                          \
  __FUNC(value::color4f)                          \
  __FUNC(value::color4d)                          \
  __FUNC(value::texcoord2h)                       \
  __FUNC(value::texcoord2f)                       \
  __FUNC(value::texcoord2d)                       \
  __FUNC(value::texcoord3h)                       \
  __FUNC(value::texcoord3f)                       \
  __FUNC(value::texcoord3d)                       \
  __FUNC(value::matrix2d)                         \
  __FUNC(value::matrix3d)                         \
  __FUNC(value::matrix4d)                         \
  __FUNC(value::frame4d) \
  __FUNC(std::vector<bool>)                                    \
  __FUNC(std::vector<value::half>)                             \
  __FUNC(std::vector<value::half2>)                            \
  __FUNC(std::vector<value::half3>)                            \
  __FUNC(std::vector<value::half4>)                            \
  __FUNC(std::vector<int32_t>)                                 \
  __FUNC(std::vector<uint32_t>)                                \
  __FUNC(std::vector<value::int2>)                             \
  __FUNC(std::vector<value::int3>)                             \
  __FUNC(std::vector<value::int4>)                             \
  __FUNC(std::vector<value::uint2>)                            \
  __FUNC(std::vector<value::uint3>)                            \
  __FUNC(std::vector<value::uint4>)                            \
  __FUNC(std::vector<int64_t>)                                 \
  __FUNC(std::vector<uint64_t>)                                \
  __FUNC(std::vector<float>)                                   \
  __FUNC(std::vector<value::float2>)                           \
  __FUNC(std::vector<value::float3>)                           \
  __FUNC(std::vector<value::float4>)                           \
  __FUNC(std::vector<double>)                                  \
  __FUNC(std::vector<value::double2>)                          \
  __FUNC(std::vector<value::double3>)                          \
  __FUNC(std::vector<value::double4>)                          \
  __FUNC(std::vector<value::quath>)                            \
  __FUNC(std::vector<value::quatf>)                            \
  __FUNC(std::vector<value::quatd>)                            \
  __FUNC(std::vector<value::normal3h>)                         \
  __FUNC(std::vector<value::normal3f>)                         \
  __FUNC(std::vector<value::normal3d>)                         \
  __FUNC(std::vector<value::vector3h>)                         \
  __FUNC(std::vector<value::vector3f>)                         \
  __FUNC(std::vector<value::vector3d>)                         \
  __FUNC(std::vector<value::point3h>)                          \
  __FUNC(std::vector<value::point3f>)                          \
  __FUNC(std::vector<value::point3d>)                          \
  __FUNC(std::vector<value::color3f>)                          \
  __FUNC(std::vector<value::color3d>)                          \
  __FUNC(std::vector<value::color4h>)                          \
  __FUNC(std::vector<value::color4f>)                          \
  __FUNC(std::vector<value::color4d>)                          \
  __FUNC(std::vector<value::texcoord2h>)                       \
  __FUNC(std::vector<value::texcoord2f>)                       \
  __FUNC(std::vector<value::texcoord2d>)                       \
  __FUNC(std::vector<value::texcoord3h>)                       \
  __FUNC(std::vector<value::texcoord3f>)                       \
  __FUNC(std::vector<value::texcoord3d>)                       \
  __FUNC(std::vector<value::matrix2d>)                         \
  __FUNC(std::vector<value::matrix3d>)                         \
  __FUNC(std::vector<value::matrix4d>)                         \
  __FUNC(std::vector<value::frame4d>)
