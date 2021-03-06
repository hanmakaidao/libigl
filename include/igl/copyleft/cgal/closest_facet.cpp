// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#include "closest_facet.h"

#include <vector>
#include <stdexcept>

#include <CGAL/AABB_tree.h>
#include <CGAL/AABB_traits.h>
#include <CGAL/AABB_triangle_primitive.h>
#include <CGAL/intersections.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>

#include "order_facets_around_edge.h"

template<
    typename DerivedV,
    typename DerivedF,
    typename DerivedI,
    typename DerivedP,
    typename DerivedR,
    typename DerivedS >
IGL_INLINE void igl::copyleft::cgal::closest_facet(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        const Eigen::PlainObjectBase<DerivedI>& I,
        const Eigen::PlainObjectBase<DerivedP>& P,
        Eigen::PlainObjectBase<DerivedR>& R,
        Eigen::PlainObjectBase<DerivedS>& S) {
    typedef CGAL::Exact_predicates_exact_constructions_kernel Kernel;
    typedef Kernel::Point_3 Point_3;
    typedef Kernel::Plane_3 Plane_3;
    typedef Kernel::Segment_3 Segment_3;
    typedef Kernel::Triangle_3 Triangle;
    typedef std::vector<Triangle>::iterator Iterator;
    typedef CGAL::AABB_triangle_primitive<Kernel, Iterator> Primitive;
    typedef CGAL::AABB_traits<Kernel, Primitive> AABB_triangle_traits;
    typedef CGAL::AABB_tree<AABB_triangle_traits> Tree;

    if (F.rows() <= 0 || I.rows() <= 0) {
        throw std::runtime_error(
                "Closest facet cannot be computed on empty mesh.");
    }

    const size_t num_faces = I.rows();
    std::vector<Triangle> triangles;
    for (size_t i=0; i<num_faces; i++) {
        const Eigen::Vector3i f = F.row(I(i, 0));
        triangles.emplace_back(
                Point_3(V(f[0], 0), V(f[0], 1), V(f[0], 2)),
                Point_3(V(f[1], 0), V(f[1], 1), V(f[1], 2)),
                Point_3(V(f[2], 0), V(f[2], 1), V(f[2], 2)));
        if (triangles.back().is_degenerate()) {
            throw std::runtime_error(
                    "Input facet components contains degenerated triangles");
        }
    }
    Tree tree(triangles.begin(), triangles.end());
    tree.accelerate_distance_queries();

    auto on_the_positive_side = [&](size_t fid, const Point_3& p) {
        const auto& f = F.row(fid).eval();
        Point_3 v0(V(f[0], 0), V(f[0], 1), V(f[0], 2));
        Point_3 v1(V(f[1], 0), V(f[1], 1), V(f[1], 2));
        Point_3 v2(V(f[2], 0), V(f[2], 1), V(f[2], 2));
        auto ori = CGAL::orientation(v0, v1, v2, p);
        switch (ori) {
            case CGAL::POSITIVE:
                return true;
            case CGAL::NEGATIVE:
                return false;
            case CGAL::COPLANAR:
                throw std::runtime_error(
                        "It seems input mesh contains self intersection");
            default:
                throw std::runtime_error("Unknown CGAL state.");
        }
        return false;
    };

    auto get_orientation = [&](size_t fid, size_t s, size_t d) -> bool {
        const auto& f = F.row(fid);
        if      ((size_t)f[0] == s && (size_t)f[1] == d) return false;
        else if ((size_t)f[1] == s && (size_t)f[2] == d) return false;
        else if ((size_t)f[2] == s && (size_t)f[0] == d) return false;
        else if ((size_t)f[0] == d && (size_t)f[1] == s) return true;
        else if ((size_t)f[1] == d && (size_t)f[2] == s) return true;
        else if ((size_t)f[2] == d && (size_t)f[0] == s) return true;
        else {
            throw std::runtime_error(
                    "Cannot compute orientation due to incorrect connectivity");
            return false;
        }
    };
    auto index_to_signed_index = [&](size_t index, bool ori) -> int{
        return (index+1) * (ori? 1:-1);
    };
    //auto signed_index_to_index = [&](int signed_index) -> size_t {
    //    return abs(signed_index) - 1;
    //};

    enum ElementType { VERTEX, EDGE, FACE };
    auto determine_element_type = [&](const Point_3& p, const size_t fid,
            size_t& element_index) {
        const auto& tri = triangles[fid];
        const Point_3 p0 = tri[0];
        const Point_3 p1 = tri[1];
        const Point_3 p2 = tri[2];

        if (p == p0) { element_index = 0; return VERTEX; }
        if (p == p1) { element_index = 1; return VERTEX; }
        if (p == p2) { element_index = 2; return VERTEX; }
        if (CGAL::collinear(p0, p1, p)) { element_index = 2; return EDGE; }
        if (CGAL::collinear(p1, p2, p)) { element_index = 0; return EDGE; }
        if (CGAL::collinear(p2, p0, p)) { element_index = 1; return EDGE; }

        element_index = 0;
        return FACE;
    };

    auto process_edge_case = [&](
            size_t query_idx,
            const size_t s, const size_t d,
            size_t preferred_facet,
            bool& orientation) {

        Point_3 mid_edge_point(
                (V(s,0) + V(d,0)) * 0.5,
                (V(s,1) + V(d,1)) * 0.5,
                (V(s,2) + V(d,2)) * 0.5);
        Point_3 query_point(
                P(query_idx, 0),
                P(query_idx, 1),
                P(query_idx, 2));

        std::vector<Tree::Primitive_id> intersected_faces;
        tree.all_intersected_primitives(Segment_3(mid_edge_point, query_point),
                std::back_inserter(intersected_faces));

        const size_t num_intersected_faces = intersected_faces.size();
        std::vector<size_t> intersected_face_indices(num_intersected_faces);
        std::vector<int> intersected_face_signed_indices(num_intersected_faces);
        std::transform(intersected_faces.begin(),
                intersected_faces.end(),
                intersected_face_indices.begin(),
                [&](const Tree::Primitive_id& itr) -> size_t
                { return I(itr-triangles.begin(), 0); });
        std::transform(
                intersected_face_indices.begin(),
                intersected_face_indices.end(),
                intersected_face_signed_indices.begin(),
                [&](size_t index) {
                    return index_to_signed_index(
                        index, get_orientation(index, s,d));
                });

        assert(num_intersected_faces >= 1);
        if (num_intersected_faces == 1) {
            // The edge must be a boundary edge.  Thus, the orientation can be
            // simply determined by checking if the query point is on the
            // positive side of the facet.
            const size_t fid = intersected_face_indices[0];
            orientation = on_the_positive_side(fid, query_point);
            return fid;
        }

        Eigen::VectorXi order;
        DerivedP pivot = P.row(query_idx).eval();
        igl::copyleft::cgal::order_facets_around_edge(V, F, s, d,
                intersected_face_signed_indices,
                pivot, order);

        // Although first and last are equivalent, make the choice based on
        // preferred_facet.
        const size_t first = order[0];
        const size_t last = order[num_intersected_faces-1];
        if (intersected_face_indices[first] == preferred_facet) {
            orientation = intersected_face_signed_indices[first] < 0;
            return intersected_face_indices[first];
        } else if (intersected_face_indices[last] == preferred_facet) {
            orientation = intersected_face_signed_indices[last] > 0;
            return intersected_face_indices[last];
        } else {
            orientation = intersected_face_signed_indices[order[0]] < 0;
            return intersected_face_indices[order[0]];
        }
    };

    auto process_face_case = [&](
            const size_t query_idx, const size_t fid, bool& orientation) {
        const auto& f = F.row(I(fid, 0));
        return process_edge_case(query_idx, f[0], f[1], I(fid, 0), orientation);
    };

    auto process_vertex_case = [&](const size_t query_idx, size_t s,
            size_t preferred_facet, bool& orientation) {
        Point_3 closest_point(V(s, 0), V(s, 1), V(s, 2));
        Point_3 query_point(P(query_idx, 0), P(query_idx, 1), P(query_idx, 2));

        std::vector<Tree::Primitive_id> intersected_faces;
        tree.all_intersected_primitives(Segment_3(closest_point, query_point),
                std::back_inserter(intersected_faces));

        const size_t num_intersected_faces = intersected_faces.size();
        std::vector<size_t> intersected_face_indices(num_intersected_faces);
        std::transform(intersected_faces.begin(),
                intersected_faces.end(),
                intersected_face_indices.begin(),
                [&](const Tree::Primitive_id& itr) -> size_t
                { return I(itr-triangles.begin(), 0); });

        std::set<size_t> adj_vertices_set;
        for (auto fid : intersected_face_indices) {
            const auto& f = F.row(fid);
            if ((size_t)f[0] != s) adj_vertices_set.insert(f[0]);
            if ((size_t)f[1] != s) adj_vertices_set.insert(f[1]);
            if ((size_t)f[2] != s) adj_vertices_set.insert(f[2]);
        }
        const size_t num_adj_vertices = adj_vertices_set.size();
        std::vector<size_t> adj_vertices(num_adj_vertices);
        std::copy(adj_vertices_set.begin(), adj_vertices_set.end(),
                adj_vertices.begin());

        std::vector<Point_3> adj_points;
        for (size_t vid : adj_vertices) {
            adj_points.emplace_back(V(vid,0), V(vid,1), V(vid,2));
        }

        // A plane is on the exterior if all adj_points lies on or to
        // one side of the plane.
        auto is_on_exterior = [&](const Plane_3& separator) {
            size_t positive=0;
            size_t negative=0;
            size_t coplanar=0;
            for (const auto& point : adj_points) {
                switch(separator.oriented_side(point)) {
                    case CGAL::ON_POSITIVE_SIDE:
                        positive++;
                        break;
                    case CGAL::ON_NEGATIVE_SIDE:
                        negative++;
                        break;
                    case CGAL::ON_ORIENTED_BOUNDARY:
                        coplanar++;
                        break;
                    default:
                        throw "Unknown plane-point orientation";
                }
            }
            auto query_orientation = separator.oriented_side(query_point);
            bool r = (positive == 0 && query_orientation == CGAL::POSITIVE)
                || (negative == 0 && query_orientation == CGAL::NEGATIVE);
            return r;
        };

        size_t d = std::numeric_limits<size_t>::max();
        for (size_t i=0; i<num_adj_vertices; i++) {
            const size_t vi = adj_vertices[i];
            for (size_t j=i+1; j<num_adj_vertices; j++) {
                Plane_3 separator(closest_point, adj_points[i], adj_points[j]);
                if (separator.is_degenerate()) {
                    throw std::runtime_error(
                            "Input mesh contains degenerated faces");
                }
                if (is_on_exterior(separator)) {
                    d = vi;
                    assert(!CGAL::collinear(
                                query_point, adj_points[i], closest_point));
                    break;
                }
            }
        }
        assert(d != std::numeric_limits<size_t>::max());

        return process_edge_case(query_idx, s, d, preferred_facet, orientation);
    };

    const size_t num_queries = P.rows();
    R.resize(num_queries, 1);
    S.resize(num_queries, 1);
    for (size_t i=0; i<num_queries; i++) {
        const Point_3 query(P(i,0), P(i,1), P(i,2));
        auto projection = tree.closest_point_and_primitive(query);
        const Point_3 closest_point = projection.first;
        size_t fid = projection.second - triangles.begin();
        bool fid_ori = false;

        // Gether all facets sharing the closest point.
        std::vector<Tree::Primitive_id> intersected_faces;
        tree.all_intersected_primitives(Segment_3(closest_point, query),
                std::back_inserter(intersected_faces));
        const size_t num_intersected_faces = intersected_faces.size();
        std::vector<size_t> intersected_face_indices(num_intersected_faces);
        std::transform(intersected_faces.begin(),
                intersected_faces.end(),
                intersected_face_indices.begin(),
                [&](const Tree::Primitive_id& itr) -> size_t
                { return I(itr-triangles.begin(), 0); });

        size_t element_index;
        auto element_type = determine_element_type(closest_point, fid,
                element_index);
        switch(element_type) {
            case VERTEX:
                {
                    const auto& f = F.row(I(fid, 0));
                    const size_t s = f[element_index];
                    fid = process_vertex_case(i, s, I(fid, 0), fid_ori);
                }
                break;
            case EDGE:
                {
                    const auto& f = F.row(I(fid, 0));
                    const size_t s = f[(element_index+1)%3];
                    const size_t d = f[(element_index+2)%3];
                    fid = process_edge_case(i, s, d, I(fid, 0), fid_ori);
                }
                break;
            case FACE:
                {
                    fid = process_face_case(i, fid, fid_ori);
                }
                break;
            default:
                throw std::runtime_error("Unknown element type.");
        }


        R(i,0) = fid;
        S(i,0) = fid_ori;
    }
}

template<
    typename DerivedV,
    typename DerivedF,
    typename DerivedP,
    typename DerivedR,
    typename DerivedS >
IGL_INLINE void igl::copyleft::cgal::closest_facet(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        const Eigen::PlainObjectBase<DerivedP>& P,
        Eigen::PlainObjectBase<DerivedR>& R,
        Eigen::PlainObjectBase<DerivedS>& S) {
    const size_t num_faces = F.rows();
    Eigen::VectorXi I(num_faces);
    I.setLinSpaced(num_faces, 0, num_faces-1);
    igl::copyleft::cgal::closest_facet(V, F, I, P, R, S);
}

#ifdef IGL_STATIC_LIBRARY
template void igl::copyleft::cgal::closest_facet<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
