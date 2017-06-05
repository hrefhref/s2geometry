// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS-IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//


// Author: ericv@google.com (Eric Veach)

#include "s2/s2boundary_operation.h"

#include <memory>
#include <gtest/gtest.h>
#include "s2/third_party/absl/memory/memory.h"
#include "s2/third_party/absl/strings/str_split.h"
#include "s2/third_party/absl/strings/strip.h"
#include "s2/s2builder.h"
#include "s2/s2builder_graph.h"
#include "s2/s2builder_layer.h"
#include "s2/s2builderutil_snap_functions.h"
#include "s2/s2shapeutil.h"
#include "s2/s2textformat.h"

namespace {

using std::make_pair;
using std::pair;
using std::unique_ptr;
using std::vector;

using Graph = S2Builder::Graph;
using GraphOptions = S2Builder::GraphOptions;
using DegenerateEdges = GraphOptions::DegenerateEdges;
using DuplicateEdges = GraphOptions::DuplicateEdges;
using SiblingPairs = GraphOptions::SiblingPairs;

using OpType = S2BoundaryOperation::OpType;
using PolygonModel = S2BoundaryOperation::PolygonModel;
using PolylineModel = S2BoundaryOperation::PolylineModel;

#if 0
PolygonModel kPolygonModels[3] = {
  PolygonModel::OPEN,
  PolygonModel::SEMI_OPEN,
  PolygonModel::CLOSED,
};

PolylineModel kPolylineModels[3] = {
  PolylineModel::OPEN,
  PolylineModel::SEMI_OPEN,
  PolylineModel::CLOSED,
};
#endif

// Returns an S2ShapeIndex containing the points, polylines, and loops (in the
// form of a single polygon) described by the following format:
//
//   point1; point2; ... # line1; line2; ... # loop1; loop2; ...
//
// Examples:
//   1:2; 2:3 # #                                 // Two points
//   # 0:0, 1:1, 2:2; 3:3, 4:4 #                 // Two polylines
//   # # 0:0, 0:3, 3:0; 1:1, 2:1, 1:2             // Two nested loops
//   5:5 # 6:6, 7:7 # 0:0, 0:1, 1:0              // One of each
//
// Loops should be directed so that the region's interior is on the left.
// Loops can be degenerate (they do not need to meet S2Loop requirements).
unique_ptr<S2ShapeIndex> MakeIndex(string const& str) {
  vector<string> strs = strings::Split(str, '#');
  DCHECK_EQ(3, strs.size()) << "Must contain two # characters: " << str;
  // TODO(ericv): Update s2textformat to understand StringPiece.
  vector<string> dim_strs[3];
  for (int d = 0; d < 3; ++d) {
    dim_strs[d] = strings::Split(strs[d], ';', strings::SkipWhitespace());
    strings::StripWhitespaceInCollection(&dim_strs[d]);
  }
  auto index = absl::MakeUnique<S2ShapeIndex>();
  vector<S2Point> points;
  for (auto const& point_str : dim_strs[0]) {
    points.push_back(s2textformat::MakePoint(point_str));
  }
  if (!points.empty()) {
    index->Add(new s2shapeutil::PointVectorShape(&points));
  }
  for (auto const& line_str : dim_strs[1]) {
    auto vertices = s2textformat::ParsePoints(line_str);
    index->Add(new s2shapeutil::LaxPolyline(vertices));
  }
  vector<vector<S2Point>> loops;
  for (auto const& loop_str : dim_strs[2]) {
    loops.push_back(s2textformat::ParsePoints(loop_str));
  }
  if (!loops.empty()) {
    index->Add(new s2shapeutil::LaxPolygon(loops));
  }
  return index;
}

S2Error::Code INDEXES_DO_NOT_MATCH = S2Error::USER_DEFINED_START;

class IndexMatchingLayer : public S2Builder::Layer {
 public:
  explicit IndexMatchingLayer(S2ShapeIndex const& index, int dimension)
      : index_(index), dimension_(dimension) {
  }
  GraphOptions graph_options() const override {
    return GraphOptions(EdgeType::DIRECTED, DegenerateEdges::KEEP,
                        DuplicateEdges::KEEP, SiblingPairs::KEEP);
  }

  void Build(Graph const& g, S2Error* error) override;

 private:
  using EdgeVector = vector<S2Shape::Edge>;
  static string ToString(EdgeVector const& edges);

  S2ShapeIndex const& index_;
  int dimension_;
};

string IndexMatchingLayer::ToString(EdgeVector const& edges) {
  string msg;
  for (auto const& edge : edges) {
    vector<S2Point> vertices = { edge.v0, edge.v1 };
    msg += s2textformat::ToString(vertices);
    msg += "; ";
  }
  return msg;
}

void IndexMatchingLayer::Build(Graph const& g, S2Error* error) {
  vector<S2Shape::Edge> actual, expected;
  for (int e = 0; e < g.num_edges(); ++e) {
    Graph::Edge const& edge = g.edge(e);
    actual.push_back(S2Shape::Edge(g.vertex(edge.first),
                                   g.vertex(edge.second)));
  }
  for (int s = 0; s < index_.num_shape_ids(); ++s) {
    S2Shape* shape = index_.shape(s);
    if (shape == nullptr || shape->dimension() != dimension_) {
      continue;
    }
    for (int e = shape->num_edges(); --e >= 0; ) {
      expected.push_back(shape->edge(e));
    }
  }
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());

  // The edges are a multiset, so we can't use std::set_difference.
  vector<S2Shape::Edge> missing, extra;
  for (auto ai = actual.begin(), ei = expected.begin();
       ai != actual.end() || ei != expected.end(); ) {
    if (ei == expected.end() || (ai != actual.end() && *ai < *ei)) {
      extra.push_back(*ai++);
    } else if (ai == actual.end() || *ei < *ai) {
      missing.push_back(*ei++);
    } else {
      ++ai;
      ++ei;
    }
  }
  if (!missing.empty() || !extra.empty()) {
    // There may be errors in more than one dimension, so we append to the
    // existing error text.
    error->Init(INDEXES_DO_NOT_MATCH,
                "%sDimension %d: Missing edges: %s Extra edges: %s\n",
                error->text().c_str(), dimension_, ToString(missing).c_str(),
                ToString(extra).c_str());
  }
}

void ExpectResult(S2BoundaryOperation::OpType op_type,
                  S2BoundaryOperation::Options const& options,
                  string const& a_str, string const& b_str,
                  string const& expected_str) {
  auto a = MakeIndex(a_str);
  auto b = MakeIndex(b_str);
  auto expected = MakeIndex(expected_str);
  S2BoundaryOperation op(op_type,
                         absl::MakeUnique<IndexMatchingLayer>(*expected, 0),
                         absl::MakeUnique<IndexMatchingLayer>(*expected, 1),
                         absl::MakeUnique<IndexMatchingLayer>(*expected, 2),
                         options);
  S2Error error;
  EXPECT_TRUE(op.Build(*a, *b, &error))
      << S2BoundaryOperation::OpTypeToString(op_type) << " failed:\n"
      << "Expected result: " << expected_str << "\n"
      << error.text();

  // Now try the same thing with boolean output.
  bool result_empty;
  S2BoundaryOperation op2(op_type, &result_empty, options);
  EXPECT_TRUE(op2.Build(*a, *b, &error)) << "Boolean "
      << S2BoundaryOperation::OpTypeToString(op_type) << " failed:\n"
      << "Expected result: " << expected_str << "\n"
      << error.text();
  EXPECT_EQ(expected->num_shape_ids() == 0, result_empty);
}

}  // namespace

// The intersections in the "expected" data below were computed in lat-lng
// space (i.e., the rectangular projection), while the actual intersections
// are computed using geodesics.  We can compensate for this by rounding the
// intersection points to a fixed precision in degrees (e.g., 2 decimals).
static S2BoundaryOperation::Options RoundToE(int exp) {
  S2BoundaryOperation::Options options;
  options.set_snap_function(s2builderutil::IntLatLngSnapFunction(exp));
  return options;
}

// TODO(ericv): Clean up or remove these notes.
//
// Options to test:
//   polygon_model:                   OPEN, SEMI_OPEN, CLOSED
//   polyline_model:                  OPEN, SEMI_OPEN, CLOSED
//   polyline_loops_have_boundaries:  true, false
//   conservative:                    true, false
//
// Geometry combinations to test:
//
// Point/point:
//  - disjoint, coincident
// Point/polyline:
//  - Start vertex, end vertex, interior vertex, degenerate polyline
//  - With polyline_loops_have_boundary: start/end vertex, degenerate polyline
// Point/polygon:
//  - Polygon interior, exterior, vertex
//  - Vertex of degenerate sibling pair shell, hole
//  - Vertex of degenerate single point shell, hole
// Polyline/polyline:
//  - Vertex intersection:
//    - Start, end, interior, degenerate, loop start/end, degenerate loop
//    - Test cases where vertex is not emitted because an incident edge is.
//  - Edge/edge: interior crossing, duplicate, reversed, degenerate
//  - Test that degenerate edges are ignored unless polyline has a single edge.
//    (For example, AA has one edge but AAA has no edges.)
// Polyline/polygon:
//  - Vertex intersection: polyline vertex cases already covered, but test
//    polygon normal vertex, sibling pair shell/hole, single vertex shell/hole
//    - Also test cases where vertex is not emitted because an edge is.
//  - Edge/edge: interior crossing, duplicate, reversed
//  - Edge/interior: polyline edge in polygon interior, exterior
// Polygon/polygon:
//  - Vertex intersection:
//    - normal vertex, sibling pair shell/hole, single vertex shell/hole
//    - Also test cases where vertex is not emitted because an edge is.
//    - Test that polygons take priority when there is a polygon vertex and
//      also isolated polyline vertices.  (There should not be any points.)
//  - Edge/edge: interior crossing, duplicate, reversed
//  - Interior/interior: polygons in interior/exterior of other polygons

TEST(S2BoundaryOperation, PointPoint) {
  S2BoundaryOperation::Options options;
  auto a = "0:0; 1:0 # #";
  auto b = "0:0; 2:0 # #";
  // Note that these results have duplicates, which is correct.  Clients can
  // eliminated the duplicates with the appropriate GraphOptions.
  ExpectResult(OpType::UNION, options, a, b,
               "0:0; 0:0; 1:0; 2:0 # #");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "0:0; 0:0 # #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "1:0 # #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "1:0; 2:0 # #");
}

TEST(S2BoundaryOperation, PointOpenPolyline) {
  // Tests operations between an open polyline and its vertices.
  //
  // The polyline "3:0, 3:0" consists of a single degenerate edge and contains
  // no points (since polyline_model() is OPEN).  Since S2BoundaryOperation
  // preserves degeneracies, this means that the union includes *both* the
  // point 3:0 and the degenerate polyline 3:0, since they do not intersect.
  S2BoundaryOperation::Options options;
  options.set_polyline_model(PolylineModel::OPEN);
  auto a = "0:0; 1:0; 2:0; 3:0 # #";
  auto b = "# 0:0, 1:0, 2:0; 3:0, 3:0 #";
  ExpectResult(OpType::UNION, options, a, b,
               "0:0; 2:0; 3:0 # 0:0, 1:0, 2:0; 3:0, 3:0 #");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "1:0 # #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "0:0; 2:0; 3:0 # #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "0:0; 2:0; 3:0 # 0:0, 1:0, 2:0; 3:0, 3:0 #");
}

TEST(S2BoundaryOperation, PointSemiOpenPolyline) {
  // Degenerate polylines are defined not contain any points under the
  // SEMI_OPEN model either, so again the point 3:0 and the degenerate
  // polyline "3:0, 3:0" do not intersect.
  S2BoundaryOperation::Options options;
  options.set_polyline_model(PolylineModel::SEMI_OPEN);
  auto a = "0:0; 1:0; 2:0; 3:0 # #";
  auto b = "# 0:0, 1:0, 2:0; 3:0, 3:0 #";
  ExpectResult(OpType::UNION, options, a, b,
               "2:0; 3:0 # 0:0, 1:0, 2:0; 3:0, 3:0 #");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "0:0; 1:0 # #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "2:0; 3:0 # #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "2:0; 3:0 # 0:0, 1:0, 2:0; 3:0, 3:0 #");
}

TEST(S2BoundaryOperation, PointClosedPolyline) {
  // Under the CLOSED model, the degenerate polyline 3:0 does contain its
  // vertex.  Since polylines take precedence over points, the union of the
  // point 3:0 and the polyline 3:0 is the polyline only.  Similarly, since
  // subtracting a point from a polyline has no effect, the symmetric
  // difference includes only the polyline objects.
  S2BoundaryOperation::Options options;
  options.set_polyline_model(PolylineModel::CLOSED);
  auto a = "0:0; 1:0; 2:0; 3:0 # #";
  auto b = "# 0:0, 1:0, 2:0; 3:0, 3:0 #";
  ExpectResult(OpType::UNION, options, a, b,
               "# 0:0, 1:0, 2:0; 3:0, 3:0 #");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "0:0; 1:0; 2:0; 3:0 # #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "# #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "# 0:0, 1:0, 2:0; 3:0, 3:0 #");
}

TEST(S2BoundaryOperation, PointPolygonInterior) {
  S2BoundaryOperation::Options options;  // PolygonModel is irrelevant.
  // One interior point and one exterior point.
  auto a = "1:1; 4:4 # #";
  auto b = "# # 0:0, 0:3, 3:0";
  ExpectResult(OpType::UNION, options, a, b,
               "4:4 # # 0:0, 0:3, 3:0");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "1:1 # #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "4:4 # #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "4:4 # # 0:0, 0:3, 3:0");
}

TEST(S2BoundaryOperation, PointOpenPolygonVertex) {
  S2BoundaryOperation::Options options;
  options.set_polygon_model(PolygonModel::OPEN);
  // See notes about the two vertices below.
  auto a = "0:1; 1:0 # #";
  auto b = "# # 0:0, 0:1, 1:0";
  ExpectResult(OpType::UNION, options, a, b,
               "0:1; 1:0 # # 0:0, 0:1, 1:0");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "# #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "0:1; 1:0 # #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "0:1; 1:0 # # 0:0, 0:1, 1:0");
}

TEST(S2BoundaryOperation, PointSemiOpenPolygonVertex) {
  S2BoundaryOperation::Options options;
  options.set_polygon_model(PolygonModel::SEMI_OPEN);
  // The two vertices are chosen such that the polygon contains one vertex but
  // not the other under PolygonModel::SEMI_OPEN.  (The same vertices are used
  // for all three PolygonModel options.)
  auto polygon = s2textformat::MakePolygon("0:0, 0:1, 1:0");
  ASSERT_TRUE(polygon->Contains(s2textformat::MakePoint("0:1")));
  ASSERT_FALSE(polygon->Contains(s2textformat::MakePoint("1:0")));
  auto a = "0:1; 1:0 # #";
  auto b = "# # 0:0, 0:1, 1:0";
  ExpectResult(OpType::UNION, options, a, b,
               "1:0 # # 0:0, 0:1, 1:0");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "0:1 # #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "1:0 # #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "1:0 # # 0:0, 0:1, 1:0");
}

TEST(S2BoundaryOperation, PointClosedPolygonVertex) {
  S2BoundaryOperation::Options options;
  options.set_polygon_model(PolygonModel::CLOSED);
// See notes about the two vertices above.
  auto a = "0:1; 1:0 # #";
  auto b = "# # 0:0, 0:1, 1:0";
  ExpectResult(OpType::UNION, options, a, b,
               "# # 0:0, 0:1, 1:0");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "0:1; 1:0 # #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "# #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "# # 0:0, 0:1, 1:0");
}

TEST(S2BoundaryOperation, PolylineVertexOpenPolylineVertex) {
  // Not implemented yet.
}

TEST(S2BoundaryOperation, PolylineVertexSemiOpenPolylineVertex) {
  // Not implemented yet.
}

TEST(S2BoundaryOperation, PolylineVertexClosedPolylineVertex) {
  // Not implemented yet.
}

// Don't bother testing every PolylineModel with every PolygonModel for vertex
// intersection, since we have already tested the PolylineModels individually
// above.  It is sufficient to use PolylineModel::SEMI_OPEN with the various
// PolygonModel options.
TEST(S2BoundaryOperation, PolylineVertexOpenPolygonVertex) {
  // Not implemented yet.
}

TEST(S2BoundaryOperation, PolylineVertexSemiOpenPolygonVertex) {
  // Not implemented yet.
}

TEST(S2BoundaryOperation, PolylineVertexClosedPolygonVertex) {
  // Not implemented yet.
}

TEST(S2BoundaryOperation, PolylineEdgePolylineEdgeCrossing) {
  // Two polyline edges that cross at a point interior to both edges.
  S2BoundaryOperation::Options options = RoundToE(1);
  auto a = "# 0:0, 2:2 #";
  auto b = "# 2:0, 0:2 #";
  ExpectResult(OpType::UNION, options, a, b,
               "# 0:0, 1:1, 2:2; 2:0, 1:1, 0:2 #");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "# 1:1, 1:1; 1:1, 1:1 #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "# 0:0, 2:2 #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "# 0:0, 1:1, 2:2; 2:0, 1:1, 0:2 #");
}

TEST(S2BoundaryOperation, PolylineEdgePolylineEdgeOverlap) {
  // The PolylineModel does not affect this calculation.  In particular the
  // intersection of a degenerate polyline edge with itself is non-empty, even
  // though the edge contains no points in the OPEN and SEMI_OPEN models.
  S2BoundaryOperation::Options options;
  options.set_polygon_model(PolygonModel::OPEN);
  // Test edges in the same and reverse directions, and degenerate edges.
  auto a = "# 0:0, 1:0, 2:0; 3:0, 3:0; 6:0, 5:0, 4:0 #";
  auto b = "# 0:0, 1:0; 3:0, 3:0; 4:0, 5:0 #";
  // As usual, the expected output includes the relevant portions of *both*
  // input polylines.  Duplicates can be removed using GraphOptions.
  ExpectResult(OpType::UNION, options, a, b,
               "# 0:0, 1:0, 2:0; 0:0, 1:0; 3:0, 3:0; 3:0, 3:0; "
               "6:0, 5:0, 4:0; 4:0, 5:0 #");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "# 0:0, 1:0; 0:0, 1:0; 3:0, 3:0; 3:0, 3:0; "
               "5:0, 4:0; 4:0, 5:0 #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "# 1:0, 2:0; 6:0, 5:0 #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "# 1:0, 2:0; 6:0, 5:0 #");
}

TEST(S2BoundaryOperation, PolylineEdgeOpenPolygonEdgeOverlap) {
  S2BoundaryOperation::Options options;
  options.set_polygon_model(PolygonModel::OPEN);
  // A polygon and two polyline edges that coincide with the polygon boundary,
  // one in the same direction and one in the reverse direction.
  auto a = "# 1:1, 1:3; 3:3, 1:3 # ";
  auto b = "# # 1:1, 1:3, 3:3, 3:1";
  ExpectResult(OpType::UNION, options, a, b,
               "# 1:1, 1:3; 3:3, 1:3 # 1:1, 1:3, 3:3, 3:1");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "# #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "# 1:1, 1:3; 3:3, 1:3 #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "# 1:1, 1:3; 3:3, 1:3 # 1:1, 1:3, 3:3, 3:1");
}

TEST(S2BoundaryOperation, PolylineEdgeSemiOpenPolygonEdgeOverlap) {
  S2BoundaryOperation::Options options;
  options.set_polygon_model(PolygonModel::SEMI_OPEN);
  auto a = "# 1:1, 1:3; 3:3, 1:3 # ";
  auto b = "# # 1:1, 1:3, 3:3, 3:1";
  ExpectResult(OpType::UNION, options, a, b,
               "# 3:3, 1:3 # 1:1, 1:3, 3:3, 3:1");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "# 1:1, 1:3 #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "# 3:3, 1:3 #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "# 3:3, 1:3 # 1:1, 1:3, 3:3, 3:1");
}

TEST(S2BoundaryOperation, PolylineEdgeClosedPolygonEdgeOverlap) {
  S2BoundaryOperation::Options options;
  options.set_polygon_model(PolygonModel::CLOSED);
  auto a = "# 1:1, 1:3; 3:3, 1:3 # ";
  auto b = "# # 1:1, 1:3, 3:3, 3:1";
  ExpectResult(OpType::UNION, options, a, b,
               "# # 1:1, 1:3, 3:3, 3:1");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "# 1:1, 1:3; 3:3, 1:3 #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "# #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "# # 1:1, 1:3, 3:3, 3:1");
}

TEST(S2BoundaryOperation, PolylineEdgePolygonInterior) {
  S2BoundaryOperation::Options options;  // PolygonModel is irrelevant.
  // One normal and one degenerate polyline edge in the polygon interior, and
  // similarly for the polygon exterior.
  auto a = "# 1:1, 2:2; 3:3, 3:3; 6:6, 7:7; 8:8, 8:8 # ";
  auto b = "# # 0:0, 0:5, 5:5, 5:0";
  ExpectResult(OpType::UNION, options, a, b,
               "# 6:6, 7:7; 8:8, 8:8 # 0:0, 0:5, 5:5, 5:0");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "# 1:1, 2:2; 3:3, 3:3 #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "# 6:6, 7:7; 8:8, 8:8 #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "# 6:6, 7:7; 8:8, 8:8 # 0:0, 0:5, 5:5, 5:0");
}

TEST(S2BoundaryOperation, PolygonVertexOpenPolygonVertex) {
  // Not implemented yet.
}

TEST(S2BoundaryOperation, PolygonVertexSemiOpenPolygonVertex) {
  // Not implemented yet.
}

TEST(S2BoundaryOperation, PolygonVertexClosedPolygonVertex) {
  // Not implemented yet.
}

TEST(S2BoundaryOperation, PolygonEdgePolygonEdgeCrossing) {
  // Two polygons whose edges cross at points interior to both edges.
  S2BoundaryOperation::Options options = RoundToE(2);
  auto a = "# # 0:0, 0:2, 2:2, 2:0";
  auto b = "# # 1:1, 1:3, 3:3, 3:1";
  ExpectResult(OpType::UNION, options, a, b,
               "# # 0:0, 0:2, 1:2, 1:3, 3:3, 3:1, 2:1, 2:0");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "# # 1:1, 1:2, 2:2, 2:1");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "# # 0:0, 0:2, 1:2, 1:1, 2:1, 2:0");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "# # 0:0, 0:2, 1:2, 1:1, 2:1, 2:0; "
               "1:2, 1:3, 3:3, 3:1, 2:1, 2:2");
}

TEST(S2BoundaryOperation, PolygonEdgeOpenPolygonEdgeOverlap) {
  S2BoundaryOperation::Options options;
  // One shape is a rectangle, the other consists of one triangle inside the
  // rectangle and one triangle outside the rectangle, where each triangle
  // shares one edge with the rectangle.  This implies that the edges are in
  // the same direction in one case and opposite directions in the other case.
  options.set_polygon_model(PolygonModel::OPEN);
  auto a = "# # 0:0, 0:4, 2:4, 2:0";
  auto b = "# # 0:0, 1:1, 2:0; 0:4, 1:5, 2:4";
  ExpectResult(OpType::UNION, options, a, b,
               "# # 0:0, 0:4, 2:4, 2:0; 0:4, 1:5, 2:4");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "# # 0:0, 1:1, 2:0");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "# # 0:0, 0:4, 2:4, 2:0, 1:1");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "# # 0:0, 0:4, 2:4, 2:0, 1:1; 0:4, 1:5, 2:4");
}

TEST(S2BoundaryOperation, PolygonEdgeSemiOpenPolygonEdgeOverlap) {
  S2BoundaryOperation::Options options;
  options.set_polygon_model(PolygonModel::SEMI_OPEN);
  auto a = "# # 0:0, 0:4, 2:4, 2:0";
  auto b = "# # 0:0, 1:1, 2:0; 0:4, 1:5, 2:4";
  ExpectResult(OpType::UNION, options, a, b,
               "# # 0:0, 0:4, 1:5, 2:4, 2:0");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "# # 0:0, 1:1, 2:0");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "# # 0:0, 0:4, 2:4, 2:0, 1:1");
  // Note that SYMMETRIC_DIFFERENCE does not guarantee that results are
  // normalized, i.e. the output could contain siblings pairs (which can be
  // discarded using S2Builder::GraphOptions).
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "# # 0:0, 0:4, 2:4, 2:0, 1:1; 0:4, 1:5, 2:4");
}

TEST(S2BoundaryOperation, PolygonEdgeClosedPolygonEdgeOverlap) {
  S2BoundaryOperation::Options options;
  options.set_polygon_model(PolygonModel::CLOSED);
  auto a = "# # 0:0, 0:4, 2:4, 2:0";
  auto b = "# # 0:0, 1:1, 2:0; 0:4, 1:5, 2:4";
  ExpectResult(OpType::UNION, options, a, b,
               "# # 0:0, 0:4, 1:5, 2:4, 2:0");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "# # 0:0, 1:1, 2:0; 0:4, 2:4");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "# # 0:0, 0:4, 2:4, 2:0, 1:1");
  // Note that SYMMETRIC_DIFFERENCE does not guarantee that results are
  // normalized, i.e. the output could contain siblings pairs.
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "# # 0:0, 0:4, 2:4, 2:0, 1:1; 0:4, 1:5, 2:4");
}

TEST(S2BoundaryOperation, PolygonPolygonInterior) {
  S2BoundaryOperation::Options options;  // PolygonModel is irrelevant.
  // One loop in the interior of another polygon and one loop in the exterior.
  auto a = "# # 0:0, 0:4, 4:4, 4:0";
  auto b = "# # 1:1, 1:2, 2:2, 2:1; 5:5, 5:6, 6:6, 6:5";
  ExpectResult(OpType::UNION, options, a, b,
               "# # 0:0, 0:4, 4:4, 4:0; 5:5, 5:6, 6:6, 6:5");
  ExpectResult(OpType::INTERSECTION, options, a, b,
               "# # 1:1, 1:2, 2:2, 2:1");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
               "# # 0:0, 0:4, 4:4, 4:0; 2:1, 2:2, 1:2, 1:1");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
               "# # 0:0, 0:4, 4:4, 4:0; 2:1, 2:2, 1:2, 1:1; "
               "5:5, 5:6, 6:6, 6:5");
}

///////////////////////////////////////////////////////////////////////////
// The remaining tests are intended to cover combinations of features or
// interesting special cases.

TEST(S2BoundaryOperation, ThreeOverlappingBars) {
  // Two vertical bars and a horizontal bar that overlaps both of the other
  // bars and connects them.

  // Round intersection points to E2 precision because the expected results
  // were computed in lat/lng space rather than using geodesics.
  S2BoundaryOperation::Options options = RoundToE(2);
  auto a = "# # 0:0, 0:2, 3:2, 3:0; 0:3, 0:5, 3:5, 3:3";
  auto b = "# # 1:1, 1:4, 2:4, 2:1";
  ExpectResult(OpType::UNION, options, a, b,
      "# # 0:0, 0:2, 1:2, 1:3, 0:3, 0:5, 3:5, 3:3, 2:3, 2:2, 3:2, 3:0");
  ExpectResult(OpType::INTERSECTION, options, a, b,
      "# # 1:1, 1:2, 2:2, 2:1; 1:3, 1:4, 2:4, 2:3");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
      "# # 0:0, 0:2, 1:2, 1:1, 2:1, 2:2, 3:2, 3:0; "
      "0:3, 0:5, 3:5, 3:3, 2:3, 2:4, 1:4, 1:3");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
      "# # 0:0, 0:2, 1:2, 1:1, 2:1, 2:2, 3:2, 3:0; "
      "0:3, 0:5, 3:5, 3:3, 2:3, 2:4, 1:4, 1:3; "
      "1:2, 1:3, 2:3, 2:2");
}

TEST(S2BoundaryOperation, FourOverlappingBars) {
  // Two vertical bars and two horizontal bars.

  // Round intersection points to E2 precision because the expected results
  // were computed in lat/lng space rather than using geodesics.
  S2BoundaryOperation::Options options = RoundToE(2);
  auto a = "# # 1:88, 1:93, 2:93, 2:88; -1:88, -1:93, 0:93, 0:88";
  auto b = "# # -2:89, -2:90, 3:90, 3:89; -2:91, -2:92, 3:92, 3:91";
  ExpectResult(OpType::UNION, options, a, b,
      "# # -1:88, -1:89, -2:89, -2:90, -1:90, -1:91, -2:91, -2:92, -1:92, "
      "-1:93, 0:93, 0:92, 1:92, 1:93, 2:93, 2:92, 3:92, 3:91, 2:91, "
      "2:90, 3:90, 3:89, 2:89, 2:88, 1:88, 1:89, 0:89, 0:88; "
      "0:90, 1:90, 1:91, 0:91" /*CW*/ );
  ExpectResult(OpType::INTERSECTION, options, a, b,
      "# # 1:89, 1:90, 2:90, 2:89; 1:91, 1:92, 2:92, 2:91; "
      "-1:89, -1:90, 0:90, 0:89; -1:91, -1:92, 0:92, 0:91");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
      "# # 1:88, 1:89, 2:89, 2:88; 1:90, 1:91, 2:91, 2:90; "
      "1:92, 1:93, 2:93, 2:92; -1:88, -1:89, 0:89, 0:88; "
      "-1:90, -1:91, 0:91, 0:90; -1:92, -1:93, 0:93, 0:92");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
      "# # 1:88, 1:89, 2:89, 2:88; -1:88, -1:89, 0:89, 0:88; "
      "1:90, 1:91, 2:91, 2:90; -1:90, -1:91, 0:91, 0:90; "
      "1:92, 1:93, 2:93, 2:92; -1:92, -1:93, 0:93, 0:92; "
      "-2:89, -2:90, -1:90, -1:89; -2:91, -2:92, -1:92, -1:91; "
      "0:89, 0:90, 1:90, 1:89; 0:91, 0:92, 1:92, 1:91; "
      "2:89, 2:90, 3:90, 3:89; 2:91, 2:92, 3:92, 3:91");
}

TEST(S2BoundaryOperation, OverlappingDoughnuts) {
  // Two overlapping square doughnuts whose holes do not overlap.
  // This means that the union polygon has only two holes rather than three.

  // Round intersection points to E2 precision because the expected results
  // were computed in lat/lng space rather than using geodesics.
  S2BoundaryOperation::Options options = RoundToE(1);
  auto a = "# # -1:-93, -1:-89, 3:-89, 3:-93; "
                      "0:-92, 2:-92, 2:-90, 0:-90" /*CW*/ ;
  auto b = "# # -3:-91, -3:-87, 1:-87, 1:-91; "
                      "-2:-90, 0:-90, 0:-88, -2:-88" /*CW*/ ;
  ExpectResult(OpType::UNION, options, a, b,
      "# # -1:-93, -1:-91, -3:-91, -3:-87, 1:-87, 1:-89, 3:-89, 3:-93; "
      "0:-92, 2:-92, 2:-90, 1:-90, 1:-91, 0:-91; " /*CW */
      "-2:-90, -1:-90, -1:-89, 0:-89, 0:-88, -2:-88" /* CW */ );
  ExpectResult(OpType::INTERSECTION, options, a, b,
      "# # -1:-91, -1:-90, 0:-90, 0:-91; "
      "0:-90, 0:-89, 1:-89, 1:-90");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
      "# # -1:-93, -1:-91, 0:-91, 0:-92, 2:-92, "
      "2:-90, 1:-90, 1:-89, 3:-89, 3:-93; "
      "-1:-90, -1:-89, 0:-89, 0:-90");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
      "# # -1:-93, -1:-91, 0:-91, 0:-92, 2:-92, "
      "2:-90, 1:-90, 1:-89, 3:-89, 3:-93; "
      "-3:-91, -3:-87, 1:-87, 1:-89, 0:-89, 0:-88,-2:-88,-2:-90,-1:-90,-1:-91; "
      "-1:-90, -1:-89, 0:-89, 0:-90; "
      "1:-91, 0:-91, 0:-90, 1:-90");
}

TEST(S2BoundaryOperation, PolylineOverlappingRectangle) {
  // A polyline that crosses from the outside to the inside of a rectangle at
  // one of its vertices.
  S2BoundaryOperation::Options options = RoundToE(1);
  auto a = "# 0:0, 2:2 #";
  auto b = "# # 1:1, 1:3, 3:3, 3:1";
  ExpectResult(OpType::UNION, options, a, b,
      "# 0:0, 1:1 # 1:1, 1:3, 3:3, 3:1");
  ExpectResult(OpType::INTERSECTION, options, a, b,
      "# 1:1, 2:2 #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
      "# 0:0, 1:1 #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
      "# 0:0, 1:1 # 1:1, 1:3, 3:3, 3:1");
}

TEST(S2BoundaryOperation, PolylineCrossingRectangleTwice) {
  // A polyline that crosses a rectangle in one direction, then moves to a
  // different side and crosses the rectangle in the other direction.  Note
  // that an extra vertex is added where the two polyline edges cross.
  S2BoundaryOperation::Options options = RoundToE(1);
  auto a = "# 0:-5, 0:5, 5:0, -5:0 #";
  auto b = "# # 1:1, 1:-1, -1:-1, -1:1";
  ExpectResult(OpType::UNION, options, a, b,
      "# 0:-5, 0:-1; 0:1, 0:5, 5:0, 1:0; -1:0, -5:0 "
      "# 1:1, 1:0, 1:-1, 0:-1, -1:-1, -1:0, -1:1, 0:1");
  ExpectResult(OpType::INTERSECTION, options, a, b,
      "# 0:-1, 0:0, 0:1; 1:0, 0:0, -1:0 #");
  ExpectResult(OpType::DIFFERENCE, options, a, b,
      "# 0:-5, 0:-1; 0:1, 0:5, 5:0, 1:0; -1:0, -5:0 #");
  ExpectResult(OpType::SYMMETRIC_DIFFERENCE, options, a, b,
      "# 0:-5, 0:-1; 0:1, 0:5, 5:0, 1:0; -1:0, -5:0 "
      "# 1:1, 1:0, 1:-1, 0:-1, -1:-1, -1:0, -1:1, 0:1");
}
