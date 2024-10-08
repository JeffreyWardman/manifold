// Copyright 2021 The Manifold Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./impl.h"

#include <algorithm>
#include <atomic>
#include <map>

#include "./hashtable.h"
#include "./mesh_fixes.h"
#include "./svd.h"
#include "manifold/parallel.h"

namespace {
using namespace manifold;

constexpr uint64_t kRemove = std::numeric_limits<uint64_t>::max();

void AtomicAddVec3(vec3& target, const vec3& add) {
  for (int i : {0, 1, 2}) {
    std::atomic<double>& tar =
        reinterpret_cast<std::atomic<double>&>(target[i]);
    double old_val = tar.load(std::memory_order_relaxed);
    while (!tar.compare_exchange_weak(old_val, old_val + add[i],
                                      std::memory_order_relaxed)) {
    }
  }
}

struct Transform4x3 {
  const mat4x3 transform;

  vec3 operator()(vec3 position) { return transform * vec4(position, 1.0); }
};

template <bool calculateTriNormal>
struct AssignNormals {
  VecView<vec3> faceNormal;
  VecView<vec3> vertNormal;
  VecView<const vec3> vertPos;
  VecView<const Halfedge> halfedges;
  const double precision;

  void operator()(const int face) {
    vec3& triNormal = faceNormal[face];

    ivec3 triVerts;
    for (int i : {0, 1, 2}) triVerts[i] = halfedges[3 * face + i].startVert;

    vec3 edge[3];
    for (int i : {0, 1, 2}) {
      const int j = (i + 1) % 3;
      edge[i] = glm::normalize(vertPos[triVerts[j]] - vertPos[triVerts[i]]);
    }

    if (calculateTriNormal) {
      triNormal = glm::normalize(glm::cross(edge[0], edge[1]));
      if (std::isnan(triNormal.x)) triNormal = vec3(0, 0, 1);
    }

    // corner angles
    vec3 phi;
    double dot = -glm::dot(edge[2], edge[0]);
    phi[0] = dot >= 1 ? 0 : (dot <= -1 ? glm::pi<double>() : std::acos(dot));
    dot = -glm::dot(edge[0], edge[1]);
    phi[1] = dot >= 1 ? 0 : (dot <= -1 ? glm::pi<double>() : std::acos(dot));
    phi[2] = glm::pi<double>() - phi[0] - phi[1];

    // assign weighted sum
    for (int i : {0, 1, 2}) {
      AtomicAddVec3(vertNormal[triVerts[i]], phi[i] * triNormal);
    }
  }
};

struct UpdateMeshID {
  const HashTableD<uint32_t> meshIDold2new;

  void operator()(TriRef& ref) { ref.meshID = meshIDold2new[ref.meshID]; }
};
}  // namespace

namespace manifold {

std::atomic<uint32_t> Manifold::Impl::meshIDCounter_(1);

uint32_t Manifold::Impl::ReserveIDs(uint32_t n) {
  return Manifold::Impl::meshIDCounter_.fetch_add(n, std::memory_order_relaxed);
}

/**
 * Create either a unit tetrahedron, cube or octahedron. The cube is in the
 * first octant, while the others are symmetric about the origin.
 */
Manifold::Impl::Impl(Shape shape, const mat4x3 m) {
  std::vector<vec3> vertPos;
  std::vector<ivec3> triVerts;
  switch (shape) {
    case Shape::Tetrahedron:
      vertPos = {{-1.0, -1.0, 1.0},
                 {-1.0, 1.0, -1.0},
                 {1.0, -1.0, -1.0},
                 {1.0, 1.0, 1.0}};
      triVerts = {{2, 0, 1}, {0, 3, 1}, {2, 3, 0}, {3, 2, 1}};
      break;
    case Shape::Cube:
      vertPos = {{0.0, 0.0, 0.0},  //
                 {0.0, 0.0, 1.0},  //
                 {0.0, 1.0, 0.0},  //
                 {0.0, 1.0, 1.0},  //
                 {1.0, 0.0, 0.0},  //
                 {1.0, 0.0, 1.0},  //
                 {1.0, 1.0, 0.0},  //
                 {1.0, 1.0, 1.0}};
      triVerts = {{1, 0, 4}, {2, 4, 0},  //
                  {1, 3, 0}, {3, 1, 5},  //
                  {3, 2, 0}, {3, 7, 2},  //
                  {5, 4, 6}, {5, 1, 4},  //
                  {6, 4, 2}, {7, 6, 2},  //
                  {7, 3, 5}, {7, 5, 6}};
      break;
    case Shape::Octahedron:
      vertPos = {{1.0, 0.0, 0.0},   //
                 {-1.0, 0.0, 0.0},  //
                 {0.0, 1.0, 0.0},   //
                 {0.0, -1.0, 0.0},  //
                 {0.0, 0.0, 1.0},   //
                 {0.0, 0.0, -1.0}};
      triVerts = {{0, 2, 4}, {1, 5, 3},  //
                  {2, 1, 4}, {3, 5, 0},  //
                  {1, 3, 4}, {0, 5, 2},  //
                  {3, 0, 4}, {2, 5, 1}};
      break;
  }
  vertPos_ = vertPos;
  for (auto& v : vertPos_) v = m * vec4(v, 1.0);
  CreateHalfedges(triVerts);
  Finish();
  InitializeOriginal();
}

void Manifold::Impl::RemoveUnreferencedVerts() {
  ZoneScoped;
  Vec<int> vertOld2New(NumVert(), 0);
  auto policy = autoPolicy(NumVert(), 1e5);
  for_each(policy, halfedge_.cbegin(), halfedge_.cend(),
           [&vertOld2New](Halfedge h) {
             reinterpret_cast<std::atomic<int>*>(&vertOld2New[h.startVert])
                 ->store(1, std::memory_order_relaxed);
           });

  const Vec<vec3> oldVertPos = vertPos_;

  Vec<size_t> tmpBuffer(oldVertPos.size());
  auto vertIdIter = TransformIterator(countAt(0_uz), [&vertOld2New](size_t i) {
    if (vertOld2New[i] > 0) return i;
    return std::numeric_limits<size_t>::max();
  });

  auto next =
      copy_if(vertIdIter, vertIdIter + tmpBuffer.size(), tmpBuffer.begin(),
              [](size_t v) { return v != std::numeric_limits<size_t>::max(); });
  if (next == tmpBuffer.end()) return;

  gather(tmpBuffer.begin(), next, oldVertPos.begin(), vertPos_.begin());

  vertPos_.resize(std::distance(tmpBuffer.begin(), next));

  exclusive_scan(vertOld2New.begin(), vertOld2New.end(), vertOld2New.begin());

  for_each(policy, halfedge_.begin(), halfedge_.end(),
           [&vertOld2New](Halfedge& h) {
             h.startVert = vertOld2New[h.startVert];
             h.endVert = vertOld2New[h.endVert];
           });
}

void Manifold::Impl::InitializeOriginal() {
  const int meshID = ReserveIDs(1);
  meshRelation_.originalID = meshID;
  auto& triRef = meshRelation_.triRef;
  triRef.resize(NumTri());
  for_each_n(autoPolicy(NumTri(), 1e5), countAt(0), NumTri(),
             [meshID, &triRef](const int tri) {
               triRef[tri] = {meshID, meshID, tri};
             });
  meshRelation_.meshIDtransform.clear();
  meshRelation_.meshIDtransform[meshID] = {meshID};
}

/**
 * Create the halfedge_ data structure from an input triVerts array like Mesh.
 */
void Manifold::Impl::CreateHalfedges(const Vec<ivec3>& triVerts) {
  ZoneScoped;
  const size_t numTri = triVerts.size();
  const int numHalfedge = 3 * numTri;
  // drop the old value first to avoid copy
  halfedge_.resize(0);
  halfedge_.resize(numHalfedge);
  Vec<uint64_t> edge(numHalfedge);
  Vec<int> ids(numHalfedge);
  auto policy = autoPolicy(numTri, 1e5);
  sequence(ids.begin(), ids.end());
  for_each_n(policy, countAt(0), numTri,
             [this, &edge, &triVerts](const int tri) {
               const ivec3& verts = triVerts[tri];
               for (const int i : {0, 1, 2}) {
                 const int j = (i + 1) % 3;
                 const int e = 3 * tri + i;
                 halfedge_[e] = {verts[i], verts[j], -1};
                 // Sort the forward halfedges in front of the backward ones
                 // by setting the highest-order bit.
                 edge[e] = uint64_t(verts[i] < verts[j] ? 1 : 0) << 63 |
                           ((uint64_t)std::min(verts[i], verts[j])) << 32 |
                           std::max(verts[i], verts[j]);
               }
             });
  // Stable sort is required here so that halfedges from the same face are
  // paired together (the triangles were created in face order). In some
  // degenerate situations the triangulator can add the same internal edge in
  // two different faces, causing this edge to not be 2-manifold. These are
  // fixed by duplicating verts in SimplifyTopology.
  stable_sort(ids.begin(), ids.end(), [&edge](const int& a, const int& b) {
    return edge[a] < edge[b];
  });

  // Mark opposed triangles for removal
  const int numEdge = numHalfedge / 2;
  for (int i = 0; i < numEdge; ++i) {
    const int pair0 = ids[i];
    Halfedge h0 = halfedge_[pair0];
    int k = i + numEdge;
    while (1) {
      const int pair1 = ids[k];
      Halfedge h1 = halfedge_[pair1];
      if (h0.startVert != h1.endVert || h0.endVert != h1.startVert) break;
      if (halfedge_[NextHalfedge(pair0)].endVert ==
          halfedge_[NextHalfedge(pair1)].endVert) {
        // Reorder so that remaining edges pair up
        if (k != i + numEdge) std::swap(ids[i + numEdge], ids[k]);
        break;
      }
      ++k;
      if (k >= numHalfedge) break;
    }
  }

  // Once sorted, the first half of the range is the forward halfedges, which
  // correspond to their backward pair at the same offset in the second half
  // of the range.
  for_each_n(policy, countAt(0), numEdge, [this, &ids, numEdge](int i) {
    const int pair0 = ids[i];
    const int pair1 = ids[i + numEdge];
    halfedge_[pair0].pairedHalfedge = pair1;
    halfedge_[pair1].pairedHalfedge = pair0;
  });

  // When opposed triangles are removed, they may strand unreferenced verts.
  RemoveUnreferencedVerts();
}

/**
 * Does a full recalculation of the face bounding boxes, including updating
 * the collider, but does not resort the faces.
 */
void Manifold::Impl::Update() {
  CalculateBBox();
  Vec<Box> faceBox;
  Vec<uint32_t> faceMorton;
  GetFaceBoxMorton(faceBox, faceMorton);
  collider_.UpdateBoxes(faceBox);
}

void Manifold::Impl::MarkFailure(Error status) {
  bBox_ = Box();
  vertPos_.resize(0);
  halfedge_.resize(0);
  vertNormal_.resize(0);
  faceNormal_.resize(0);
  halfedgeTangent_.resize(0);
  meshRelation_ = MeshRelationD();
  status_ = status;
}

void Manifold::Impl::Warp(std::function<void(vec3&)> warpFunc) {
  WarpBatch([&warpFunc](VecView<vec3> vecs) {
    for_each(ExecutionPolicy::Seq, vecs.begin(), vecs.end(), warpFunc);
  });
}

void Manifold::Impl::WarpBatch(std::function<void(VecView<vec3>)> warpFunc) {
  warpFunc(vertPos_.view());
  CalculateBBox();
  if (!IsFinite()) {
    MarkFailure(Error::NonFiniteVertex);
    return;
  }
  Update();
  faceNormal_.resize(0);  // force recalculation of triNormal
  CalculateNormals();
  SetPrecision();
  InitializeOriginal();
  Finish();
}

Manifold::Impl Manifold::Impl::Transform(const mat4x3& transform_) const {
  ZoneScoped;
  if (transform_ == mat4x3(1.0)) return *this;
  auto policy = autoPolicy(NumVert());
  Impl result;
  if (status_ != Manifold::Error::NoError) {
    result.status_ = status_;
    return result;
  }
  result.collider_ = collider_;
  result.meshRelation_ = meshRelation_;
  result.precision_ = precision_;
  result.bBox_ = bBox_;
  result.halfedge_ = halfedge_;
  result.halfedgeTangent_.resize(halfedgeTangent_.size());

  result.meshRelation_.originalID = -1;
  for (auto& m : result.meshRelation_.meshIDtransform) {
    m.second.transform = transform_ * mat4(m.second.transform);
  }

  result.vertPos_.resize(NumVert());
  result.faceNormal_.resize(faceNormal_.size());
  result.vertNormal_.resize(vertNormal_.size());
  transform(vertPos_.begin(), vertPos_.end(), result.vertPos_.begin(),
            Transform4x3({transform_}));

  mat3 normalTransform = NormalTransform(transform_);
  transform(faceNormal_.begin(), faceNormal_.end(), result.faceNormal_.begin(),
            TransformNormals({normalTransform}));
  transform(vertNormal_.begin(), vertNormal_.end(), result.vertNormal_.begin(),
            TransformNormals({normalTransform}));

  const bool invert = glm::determinant(mat3(transform_)) < 0;

  if (halfedgeTangent_.size() > 0) {
    for_each_n(policy, countAt(0), halfedgeTangent_.size(),
               TransformTangents({result.halfedgeTangent_, 0, mat3(transform_),
                                  invert, halfedgeTangent_, halfedge_}));
  }

  if (invert) {
    for_each_n(policy, countAt(0), result.NumTri(),
               FlipTris({result.halfedge_}));
  }

  // This optimization does a cheap collider update if the transform is
  // axis-aligned.
  if (!result.collider_.Transform(transform_)) result.Update();

  result.CalculateBBox();
  // Scale the precision by the norm of the 3x3 portion of the transform.
  result.precision_ *= SpectralNorm(mat3(transform_));
  // Maximum of inherited precision loss and translational precision loss.
  result.SetPrecision(result.precision_);
  return result;
}

/**
 * Sets the precision based on the bounding box, and limits its minimum value
 * by the optional input.
 */
void Manifold::Impl::SetPrecision(double minPrecision) {
  precision_ = MaxPrecision(minPrecision, bBox_);
}

/**
 * If face normals are already present, this function uses them to compute
 * vertex normals (angle-weighted pseudo-normals); otherwise it also computes
 * the face normals. Face normals are only calculated when needed because
 * nearly degenerate faces will accrue rounding error, while the Boolean can
 * retain their original normal, which is more accurate and can help with
 * merging coplanar faces.
 *
 * If the face normals have been invalidated by an operation like Warp(),
 * ensure you do faceNormal_.resize(0) before calling this function to force
 * recalculation.
 */
void Manifold::Impl::CalculateNormals() {
  ZoneScoped;
  vertNormal_.resize(NumVert());
  auto policy = autoPolicy(NumTri(), 1e4);
  fill(vertNormal_.begin(), vertNormal_.end(), vec3(0));
  bool calculateTriNormal = false;
  if (faceNormal_.size() != NumTri()) {
    faceNormal_.resize(NumTri());
    calculateTriNormal = true;
  }
  if (calculateTriNormal)
    for_each_n(policy, countAt(0), NumTri(),
               AssignNormals<true>({faceNormal_, vertNormal_, vertPos_,
                                    halfedge_, precision_}));
  else
    for_each_n(policy, countAt(0), NumTri(),
               AssignNormals<false>({faceNormal_, vertNormal_, vertPos_,
                                     halfedge_, precision_}));
  for_each(policy, vertNormal_.begin(), vertNormal_.end(),
           [](vec3& v) { v = SafeNormalize(v); });
}

/**
 * Remaps all the contained meshIDs to new unique values to represent new
 * instances of these meshes.
 */
void Manifold::Impl::IncrementMeshIDs() {
  HashTable<uint32_t> meshIDold2new(meshRelation_.meshIDtransform.size() * 2);
  // Update keys of the transform map
  std::map<int, Relation> oldTransforms;
  std::swap(meshRelation_.meshIDtransform, oldTransforms);
  const int numMeshIDs = oldTransforms.size();
  int nextMeshID = ReserveIDs(numMeshIDs);
  for (const auto& pair : oldTransforms) {
    meshIDold2new.D().Insert(pair.first, nextMeshID);
    meshRelation_.meshIDtransform[nextMeshID++] = pair.second;
  }

  const size_t numTri = NumTri();
  for_each_n(autoPolicy(numTri, 1e5), meshRelation_.triRef.begin(), numTri,
             UpdateMeshID({meshIDold2new.D()}));
}

/**
 * Returns a sparse array of the bounding box overlaps between the edges of
 * the input manifold, Q and the faces of this manifold. Returned indices only
 * point to forward halfedges.
 */
SparseIndices Manifold::Impl::EdgeCollisions(const Impl& Q,
                                             bool inverted) const {
  ZoneScoped;
  Vec<TmpEdge> edges = CreateTmpEdges(Q.halfedge_);
  const size_t numEdge = edges.size();
  Vec<Box> QedgeBB(numEdge);
  const auto& vertPos = Q.vertPos_;
  auto policy = autoPolicy(numEdge, 1e5);
  for_each_n(
      policy, countAt(0), numEdge, [&QedgeBB, &edges, &vertPos](const int e) {
        QedgeBB[e] = Box(vertPos[edges[e].first], vertPos[edges[e].second]);
      });

  SparseIndices q1p2(0);
  if (inverted)
    q1p2 = collider_.Collisions<false, true>(QedgeBB.cview());
  else
    q1p2 = collider_.Collisions<false, false>(QedgeBB.cview());

  if (inverted)
    for_each(policy, countAt(0_uz), countAt(q1p2.size()),
             ReindexEdge<true>({edges, q1p2}));
  else
    for_each(policy, countAt(0_uz), countAt(q1p2.size()),
             ReindexEdge<false>({edges, q1p2}));
  return q1p2;
}

/**
 * Returns a sparse array of the input vertices that project inside the XY
 * bounding boxes of the faces of this manifold.
 */
SparseIndices Manifold::Impl::VertexCollisionsZ(VecView<const vec3> vertsIn,
                                                bool inverted) const {
  ZoneScoped;
  if (inverted)
    return collider_.Collisions<false, true>(vertsIn);
  else
    return collider_.Collisions<false, false>(vertsIn);
}

bool Manifold::Impl::Internal(int halfedge) const {
  const Halfedge edge = halfedge_[halfedge];
  const Halfedge pair = halfedge_[edge.pairedHalfedge];
  const int edgeFace = halfedge / 3;
  const int pairFace = edge.pairedHalfedge / 3;

  if (meshRelation_.triRef[edgeFace].meshID !=
      meshRelation_.triRef[pairFace].meshID)
    return false;

  const vec3 base = vertPos_[edge.startVert];
  const int baseNum = halfedge - 3 * edgeFace;
  const int jointNum = edge.pairedHalfedge - 3 * pairFace;

  if (!meshRelation_.triProperties.empty()) {
    const ivec3 triProp0 = meshRelation_.triProperties[edgeFace];
    const ivec3 triProp1 = meshRelation_.triProperties[pairFace];
    if (triProp0[baseNum] != triProp1[Next3(jointNum)] ||
        triProp0[Next3(baseNum)] != triProp1[jointNum])
      return false;
  }

  const int edgeNum = Prev3(baseNum);
  const int pairNum = Prev3(jointNum);
  const vec3 jointVec = vertPos_[pair.startVert] - base;
  const vec3 edgeVec =
      vertPos_[halfedge_[3 * edgeFace + edgeNum].startVert] - base;
  const vec3 pairVec =
      vertPos_[halfedge_[3 * pairFace + pairNum].startVert] - base;

  const double length = std::max(glm::length(jointVec), glm::length(edgeVec));
  const double lengthPair =
      std::max(glm::length(jointVec), glm::length(pairVec));
  vec3 normal = glm::cross(jointVec, edgeVec);
  const double area = glm::length(normal);
  const double areaPair = glm::length(glm::cross(pairVec, jointVec));
  // Don't link degenerate triangles
  if (area < length * precision_ || areaPair < lengthPair * precision_)
    return false;

  const double volume = std::abs(glm::dot(normal, pairVec));
  // Only operate on coplanar triangles
  return volume < std::max(area, areaPair) * precision_;
}

Vec<bool> Manifold::Impl::InternalEdges() const {
  Vec<bool> edgeInternal(halfedge_.size(), false);
  for_each_n(autoPolicy(halfedge_.size()), countAt(0), halfedge_.size(),
             [this, &edgeInternal](size_t h) {
               const Halfedge edge = halfedge_[h];
               if (!edge.IsForward()) return;

               const bool internal = Internal(h);
               edgeInternal[h] = internal;
               edgeInternal[edge.pairedHalfedge] = internal;
             });
  return edgeInternal;
}

}  // namespace manifold
