// SPDX-License-Identifier: MIT
// Copyright 2022 - Present, Syoyo Fujita.
#pragma once

#include <string>
#include <unordered_set>

//
#include "nonstd/optional.hpp"
//
#include "crate-format.hh"
#include "stream-reader.hh"

namespace tinyusdz {
namespace crate {

struct CrateReaderConfig {
  int numThreads = -1;

  // For malcious Crate data.
  // Set limits to prevent infinite-loop, buffer-overrun, out-of-memory, etc.
  size_t maxTOCSections = 32;

  size_t maxNumTokens = 1024*1024;
  size_t maxNumStrings = 1024*1024;
  size_t maxNumFields = 1024*1024;
  size_t maxNumFieldSets = 1024*1024;
  size_t maxNumSpecifiers = 1024*1024;
  size_t maxNumPaths = 1024*1024;

  size_t maxNumIndices = 1024*1024*16;
  size_t maxDictElements = 256;
  size_t maxArrayElements = 1024*1024*1024; // 1G
  size_t maxAssetPathElements = 512;

  size_t maxTokenLength = 4096; // Maximum allowed length of `token` string
  size_t maxStringLength = 1024*1024*64;

  // Total memory budget for uncompressed USD data(vertices, `tokens`, ...)` in [bytes].
  size_t maxMemoryBudget = std::numeric_limits<int32_t>::max(); // Default 2GB
};

///
/// Crate(binary data) reader
///
class CrateReader {
 public:

  ///
  /// Intermediate Node data structure for scene graph.
  /// This does not contain actual prim/property data.
  ///
  class Node {
   public:
    // -2 = initialize as invalid node
    Node() : _parent(-2) {}

    Node(int64_t parent, Path &path) : _parent(parent), _path(path) {}

    int64_t GetParent() const { return _parent; }

    const std::vector<size_t> &GetChildren() const { return _children; }

    ///
    /// child_name is used when reconstructing scene graph.
    ///
    void AddChildren(const std::string &child_name, size_t node_index) {
      assert(_primChildren.count(child_name) == 0);
      _primChildren.emplace(child_name);
      _children.push_back(node_index);
    }

    ///
    /// Get full path(e.g. `/muda/dora/bora` when the parent is `/muda/dora` and
    /// this node is `bora`)
    ///
    // std::string GetFullPath() const { return _path.full_path_name(); }

    ///
    /// Get local path
    ///
    std::string GetLocalPath() const { return _path.full_path_name(); }


    ///
    /// Element Path(= name of Prim. Tokens in `primChildren` field). Prim node only.
    ///
    void SetElementPath(Path &path) {
      _elemPath = path;
    }

    nonstd::optional<std::string> GetElementName() const  {
      if (_elemPath.IsRelativePath()) {
        return _elemPath.full_path_name();
      } else {
        return nonstd::nullopt;
      }
    }

    // Element path(e.g. `geom0`)
    const Path &GetElementPath() const  {
      return _elemPath;
    }

    // Full path(e.g. `/root/geom0`
    const Path &GetPath() const { return _path; }

    // crate::CrateDataType GetNodeDataType() const { return _node_type; }

    const std::unordered_set<std::string> &GetPrimChildren() const {
      return _primChildren;
    }

    //void SetAssetInfo(const value::dict &dict) { _assetInfo = dict; }
    //const value::dict &GetAssetInfo() const { return _assetInfo; }

   private:
    int64_t
        _parent;  // -1 = this node is the root node. -2 = invalid or leaf node
    std::vector<size_t> _children;  // index to child nodes.
    std::unordered_set<std::string>
        _primChildren;  // List of name of child nodes

    Path _path;  // local path
    //value::dict _assetInfo;
    Path _elemPath;

    // value::TypeId _node_type;
    // NodeType _node_type;
  };

 public:

 private:
  CrateReader() = delete;

 public:
  CrateReader(StreamReader *sr, const CrateReaderConfig &config = CrateReaderConfig());
  ~CrateReader();

  bool ReadBootStrap();
  bool ReadTOC();

  ///
  /// Read TOC section
  ///
  bool ReadSection(crate::Section *s);

  // Read known sections
  bool ReadPaths();
  bool ReadTokens();
  bool ReadStrings();
  bool ReadFields();
  bool ReadFieldSets();
  bool ReadSpecs();

  bool BuildLiveFieldSets();

  std::string GetError();
  std::string GetWarning();

  // Approximated memory usage in [mb]
  size_t GetMemoryUsageInMB() const {
    return size_t(_memoryUsage / 1024 / 1024);
  }

  /// -------------------------------------
  /// Following Methods are valid after successfull parsing of Crate data.
  ///
  size_t NumNodes() const {
    return _nodes.size();
  }

  const std::vector<Node> GetNodes() const {
    return _nodes;
  }

  const std::vector<value::token> GetTokens() const {
    return _tokens;
  }

  const std::vector<crate::Index> GetStringIndices() const {
    return _string_indices;
  }

  const std::vector<crate::Field> &GetFields() const {
    return _fields;
  }

  const std::vector<crate::Index> &GetFieldsetIndices() const {
    return _fieldset_indices;
  }

  const std::vector<Path> &GetPaths() const {
    return _paths;
  }

  const std::vector<Path> &GetElemPaths() const {
    return _elemPaths;
  }

  const std::vector<crate::Spec> &GetSpecs() const {
    return _specs;
  }

  
  const std::map<crate::Index, FieldValuePairVector> &GetLiveFieldSets() const {
    return _live_fieldsets;
  }

#if 0
  // FIXME: May not need this
  const std::vector<Path> &GetPaths() const {
    return _paths;
  }
#endif



  const nonstd::optional<value::token> GetToken(crate::Index token_index) const;
  const nonstd::optional<value::token> GetStringToken(
      crate::Index string_index) const;

  bool HasField(const std::string &key) const;
  nonstd::optional<crate::Field> GetField(crate::Index index) const;
  nonstd::optional<std::string> GetFieldString(crate::Index index) const;
  nonstd::optional<std::string> GetSpecString(crate::Index index) const;

  size_t NumPaths() const {
    return _paths.size();
  }

  nonstd::optional<Path> GetPath(crate::Index index) const;
  nonstd::optional<Path> GetElementPath(crate::Index index) const;
  nonstd::optional<std::string> GetPathString(crate::Index index) const;



  ///
  /// Find if a field with (`name`, `tyname`) exists in FieldValuePairVector.
  ///
  bool HasFieldValuePair(const FieldValuePairVector &fvs,
                         const std::string &name, const std::string &tyname);

  ///
  /// Find if a field with `name`(type can be arbitrary) exists in
  /// FieldValuePairVector.
  ///
  bool HasFieldValuePair(const FieldValuePairVector &fvs,
                         const std::string &name);

  nonstd::expected<FieldValuePair, std::string> GetFieldValuePair(
      const FieldValuePairVector &fvs, const std::string &name,
      const std::string &tyname);

  nonstd::expected<FieldValuePair, std::string> GetFieldValuePair(
      const FieldValuePairVector &fvs, const std::string &name);

  //bool ParseAttribute(const FieldValuePairVector &fvs,
  //                                  PrimAttrib *attr,
  //                                  const std::string &prop_name);

 private:


  bool BuildDecompressedPathsImpl(
      std::vector<uint32_t> const &pathIndexes,
      std::vector<int32_t> const &elementTokenIndexes,
      std::vector<int32_t> const &jumps, size_t curIndex, Path parentPath);

  bool UnpackValueRep(const crate::ValueRep &rep, crate::CrateValue *value);
  bool UnpackInlinedValueRep(const crate::ValueRep &rep,
                             crate::CrateValue *value);

  //
  // Construct node hierarchy.
  //
  bool BuildNodeHierarchy(std::vector<uint32_t> const &pathIndexes,
                          std::vector<int32_t> const &elementTokenIndexes,
                          std::vector<int32_t> const &jumps, size_t curIndex,
                          int64_t parentNodeIndex);

  bool ReadCompressedPaths(const uint64_t ref_num_paths);


  template <class Int>
  bool ReadCompressedInts(Int *out, size_t num_elements);

  bool ReadIndices(std::vector<crate::Index> *is);
  bool ReadIndex(crate::Index *i);
  bool ReadString(std::string *s);
  bool ReadValueRep(crate::ValueRep *rep);

  bool ReadPathArray(std::vector<Path> *d);
  bool ReadStringArray(std::vector<std::string> *d);

  // customData(Dictionary)
  bool ReadCustomData(CustomDataType *d);

  bool ReadTimeSamples(value::TimeSamples *d);

  // integral array
  template <typename T>
  bool ReadIntArray(bool is_compressed, std::vector<T> *d);

  bool ReadHalfArray(bool is_compressed, std::vector<value::half> *d);
  bool ReadFloatArray(bool is_compressed, std::vector<float> *d);
  bool ReadDoubleArray(bool is_compressed, std::vector<double> *d);

  // TODO: Templatize
  bool ReadPathListOp(ListOp<Path> *d);
  bool ReadTokenListOp(ListOp<value::token> *d);
  bool ReadStringListOp(ListOp<std::string> *d);
  bool ReadIntListOp(ListOp<int32_t> *d);
  bool ReadUIntListOp(ListOp<uint32_t> *d);
  bool ReadInt64ListOp(ListOp<int64_t> *d);
  bool ReadUInt64ListOp(ListOp<uint64_t> *d);

  // Read 64bit uint with range check
  bool ReadNum(uint64_t &n, uint64_t maxnum);

  // Header(bootstrap)
  uint8_t _version[3] = {0, 0, 0};

  crate::TableOfContents _toc;

  int64_t _toc_offset{0};

  // index to _toc.sections
  int64_t _tokens_index{-1};
  int64_t _paths_index{-1};
  int64_t _strings_index{-1};
  int64_t _fields_index{-1};
  int64_t _fieldsets_index{-1};
  int64_t _specs_index{-1};

  std::vector<value::token> _tokens;
  std::vector<crate::Index> _string_indices;
  std::vector<crate::Field> _fields;
  std::vector<crate::Index> _fieldset_indices;
  std::vector<crate::Spec> _specs;
  std::vector<Path> _paths;
  std::vector<Path> _elemPaths; 

  std::vector<Node> _nodes;  // [0] = root node
                             //
  // `_live_fieldsets` contains unpacked value keyed by fieldset index.
  // Used for reconstructing Scene object
  // TODO(syoyo): Use unordered_map?
  std::map<crate::Index, FieldValuePairVector>
      _live_fieldsets;  // <fieldset index, List of field with unpacked Values>

  // class Impl;
  // Impl *_impl;

  const StreamReader *_sr{};

  void PushError(const std::string &s) const {
    _err += s;
  }
  void PushWarn(const std::string &s) const {
    _warn += s;
  }
  mutable std::string _err;
  mutable std::string _warn;

  CrateReaderConfig _config;

  // Approximated uncompressed memory usage(vertices, `tokens`, ...) in bytes. 
  uint64_t _memoryUsage{0};
};

}  // namespace crate
}  // namespace tinyusdz
