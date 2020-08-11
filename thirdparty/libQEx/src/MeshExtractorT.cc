/*
 * Copyright 2013 Computer Graphics Group, RWTH Aachen University
 * Authors: David Bommes <bommes@cs.rwth-aachen.de>
 *          Hans-Christian Ebke <ebke@cs.rwth-aachen.de>
 *
 * This file is part of QEx.
 *
 * QEx is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * QEx is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with QEx.  If not, see <http://www.gnu.org/licenses/>.
 */

#define QEX_QUADMESHEXTRACTORT_C
#ifndef NDEBUG
#define DEBUG_VERBOSITY 2
#endif

//== INCLUDES =================================================================

#include "MeshExtractorT.hh"
#include "MeshDecimatorT.hh"

#include <math.h>
#include <cmath>
#include <sstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <OpenMesh/Core/Utils/PropertyManager.hh>
#include <iostream>

#define ROUND_QME(x) ((x)<0?int((x)-0.5):int((x)+0.5))

namespace QEx {

namespace {
template<class TMeshT>
class LEIPointerEquality {
    public:
        LEIPointerEquality(typename MeshExtractorT<TMeshT>::LocalEdgeInfo &b) : b(b) {}
        bool operator() (const typename MeshExtractorT<TMeshT>::LocalEdgeInfo &a) {
            return &a == &b;
        }

    private:
        const typename MeshExtractorT<TMeshT>::LocalEdgeInfo &b;
};
}

namespace {
template<class TMeshT>
class OriginalEmbedding {
    public:
        OriginalEmbedding(TMeshT &mesh) : mesh(mesh) {}

        typename TMeshT::Point operator() (typename TMeshT::HalfedgeHandle heh) {
            return mesh.point(mesh.to_vertex_handle(heh));
        }
        TMeshT &mesh;
};

template<class TMeshT>
class HeVectorEmbedding {
    public:
        HeVectorEmbedding(std::vector<typename TMeshT::Point> &embedding) :
            embedding(embedding) {}

        typename TMeshT::Point operator() (typename TMeshT::HalfedgeHandle heh) {
            return embedding[heh.idx()];
        }
        std::vector<typename TMeshT::Point> &embedding;
};
}

template<class TMeshT>
template<class PolyMeshT>
void MeshExtractorT<TMeshT>::extract(std::vector<double>& _uv_coords,
        typename PropMgr<PolyMeshT>::LocalUvsPropertyManager &heLocalUvProp,
        PolyMeshT& _quad_mesh, const std::vector<unsigned int> * const _external_valences) {

    // --------------------------------------------------------
    // 1. collapse degenerate edges prior to truncation
    // --------------------------------------------------------
    std::vector<unsigned int> external_valences;
    if (_external_valences)
        external_valences = *_external_valences;
    std::vector<double> uv_coords = _uv_coords;

    std::vector<typename TMeshT::Point> he_points;
    he_points.reserve(tri_mesh_.n_halfedges());
    for (typename TMeshT::HalfedgeIter he_it = tri_mesh_.halfedges_begin(),
            he_end = tri_mesh_.halfedges_end(); he_it != he_end; ++he_it) {
        he_points.push_back(
                tri_mesh_.point(tri_mesh_.to_vertex_handle(*he_it)));
    }

    MeshDecimator<TMeshT> decimator(tri_mesh_, uv_coords,
                                    external_valences);
    bool decimated = decimator.decimate();

    // --------------------------------------------------------
    // 2. extract transition functions
    // --------------------------------------------------------
    extract_transition_functions(uv_coords);

    // --------------------------------------------------------
    // 3. preprocess uv_coords in order to represent it exact
    // --------------------------------------------------------
    consistent_truncation(uv_coords);

    // --------------------------------------------------------
    // 4. collapse degenerate edges again after truncation
    // --------------------------------------------------------
    decimated |= decimator.decimate();

    // --------------------------------------------------------
    // 5. generate quadmesh-vertices (and local edge information)
    // --------------------------------------------------------
    if (decimated) {
        generate_vertices(
                uv_coords,
                _external_valences ? &external_valences : 0,
                HeVectorEmbedding<TMeshT>(he_points));
    } else {
        generate_vertices(
                uv_coords,
                _external_valences ? &external_valences : 0,
                OriginalEmbedding<TMeshT>(tri_mesh_));
    }

    // --------------------------------------------------------
    // 6. generate quadmesh-edges
    // --------------------------------------------------------
    generate_connections(uv_coords);

    try_connect_incomplete_gvertices();

    // --------------------------------------------------------
    // 7. traverse faces and store result in _quad_mesh
    // --------------------------------------------------------
    generate_faces_and_store_quadmesh(_quad_mesh, heLocalUvProp);

    //print_quad_mesh_metrics(_quad_mesh);
}

/// Constructor
template<class TMeshT>
MeshExtractorT<TMeshT>::MeshExtractorT(const TMesh& _tri_mesh) :
        tri_mesh_(_tri_mesh), du_(1, 0), dv_(0, 1) {
    // CCW cartesian orientations
    cartesian_orientations_.push_back(du_);
    cartesian_orientations_.push_back(dv_);
    cartesian_orientations_.push_back(-du_);
    cartesian_orientations_.push_back(-dv_);
}

/// Destructor
template<class TMeshT>
MeshExtractorT<TMeshT>::~MeshExtractorT() {
}

template<class TMeshT>
void MeshExtractorT<TMeshT>::extract_transition_functions(const std::vector<double>& _uv_coords) {

    tf_.resize(tri_mesh_.n_edges());

    for (EIter e_it = tri_mesh_.edges_sbegin(), e_end = tri_mesh_.edges_end(); e_it != e_end; ++e_it)
        if (tri_mesh_.is_boundary(*e_it)) {
            tf_[e_it->idx()] = TF(0, 0, 0);
        } else {
            HEH heh0 = tri_mesh_.halfedge_handle(*e_it, 0);
            HEH heh1 = tri_mesh_.halfedge_handle(*e_it, 1);
            HEH heh0p = tri_mesh_.prev_halfedge_handle(heh0);
            HEH heh1p = tri_mesh_.prev_halfedge_handle(heh1);

            Complex l0 = uv_as_complex(heh0, _uv_coords);
            Complex l1 = uv_as_complex(heh0p, _uv_coords);
            Complex r0 = uv_as_complex(heh1p, _uv_coords);
            Complex r1 = uv_as_complex(heh1, _uv_coords);

            // compute rotational part via complex numbers
            int r = ROUND_QME(2.0*std::log((r0-r1)/(l0-l1)).imag()/M_PI);
            r = ((r % 4) + 4) % 4; // assure that r is between 0 and 3
            // compute translational part
            const Complex t = r0 - std::pow(Complex(0, 1), r) * l0;
            // push back new transition function
            tf_[e_it->idx()] = TF(r, ROUND_QME(t.real()), ROUND_QME(t.imag()));
        }
}


//-----------------------------------------------------------------------------


template<class TMeshT>
void MeshExtractorT<TMeshT>::consistent_truncation(
        std::vector<double>& _uv_coords) {
    if (tri_mesh_.has_edge_status()) {
        // correct integer values at boundaries
        for (EIter e_it = tri_mesh_.edges_sbegin(), e_end = tri_mesh_.edges_end(); e_it != e_end; ++e_it) {
            if (tri_mesh_.is_boundary(*e_it)) {
                if (tri_mesh_.status(*e_it).selected() || tri_mesh_.status(*e_it).feature()) {
                    const HEH heh0 = tri_mesh_.halfedge_handle(*e_it, 0);
                    const HEH heh1 = tri_mesh_.halfedge_handle(*e_it, 1);

                    for (unsigned int i = 0; i < 2; ++i) {
                        if (fabs(_uv_coords[2 * heh0.idx() + i] - ROUND_QME(_uv_coords[2*heh0.idx()+i])) < 1e-4 &&
                                fabs(_uv_coords[2 * heh1.idx() + i] - ROUND_QME(_uv_coords[2*heh1.idx()+i])) < 1e-4) {
                            _uv_coords[2 * heh0.idx() + i] = ROUND_QME(_uv_coords[2*heh0.idx()+i]);
                            _uv_coords[2 * heh1.idx() + i] = ROUND_QME(_uv_coords[2*heh1.idx()+i]);
                        }
                    }
                }
            }
        }
    }

    // for all vertices
    for (VIter v_it = tri_mesh_.vertices_sbegin(), v_end = tri_mesh_.vertices_end(); v_it != v_end; ++v_it) {
        // for all incoming halfedges
        double max_u_abs(0), max_trans_abs(0);
        for (CVIHIter vih_it = tri_mesh_.cvih_iter(*v_it); vih_it.is_valid(); ++vih_it) {
            const HEH heh = *vih_it;

            if (!tri_mesh_.is_boundary(heh)) {
                max_u_abs = std::max(max_u_abs,
                        fabs(_uv_coords[2 * heh.idx()]));
                max_u_abs = std::max(max_u_abs,
                        fabs(_uv_coords[2 * heh.idx() + 1]));

                if (!tri_mesh_.is_boundary(
                        tri_mesh_.opposite_halfedge_handle(heh))) {
                    EH eh = tri_mesh_.edge_handle(heh);
                    max_trans_abs = std::max(max_trans_abs,
                            double(abs(tf_[eh.idx()].tu)));
                    max_trans_abs = std::max(max_trans_abs,
                            double(abs(tf_[eh.idx()].tv)));
                }
            }
        }

        // update u to correct precision
        double max_v = max_u_abs + max_trans_abs + 1;
        max_v = pow(2.0, std::ceil(log(max_v) / log(2.0)) + 1);

        // clear critical bits of start vertex
        CVIHIter vih_it = tri_mesh_.cvih_iter(*v_it);
        const HEH heh = *vih_it;
        _uv_coords[2 * heh.idx()] += max_v;
        _uv_coords[2 * heh.idx()] -= max_v;
        _uv_coords[2 * heh.idx() + 1] += max_v;
        _uv_coords[2 * heh.idx() + 1] -= max_v;

        // get transition around vertex
        TF vtrans = transition(*v_it);

        // correct dependency between coordinates of irregular vertices
        if (!tri_mesh_.is_boundary(*v_it) && vtrans != TF::identity()) {
            assert(vtrans.r >= 0 && vtrans.r <= 3);

            switch (vtrans.r) {
                case 1:
                    _uv_coords[2 * heh.idx()] = double(vtrans.tu - vtrans.tv) / 2.0;
                    _uv_coords[2 * heh.idx() + 1] = double(vtrans.tu + vtrans.tv) / 2.0;
                    break;
                case 2:
                    _uv_coords[2 * heh.idx()] = double(vtrans.tu) / 2.0;
                    _uv_coords[2 * heh.idx() + 1] = double(vtrans.tv) / 2.0;
                    break;
                case 3:
                    _uv_coords[2 * heh.idx()] = double(vtrans.tu + vtrans.tv) / 2.0;
                    _uv_coords[2 * heh.idx() + 1] = double(vtrans.tv - vtrans.tu) / 2.0;
                    break;
                default:
                    if (vtrans.r != 0 || abs(vtrans.tu) + abs(vtrans.tv) > 1)
                        std::cerr
                                << "ERROR: non-identity transition function cannot have a different rotation than 1,2,3: "
                                << vtrans.r << ", " << vtrans.tu << ", "
                                << vtrans.tv << std::endl;
                    break;
            }
        }

        // propagate in one-ring
        double u_cur = _uv_coords[2 * heh.idx()];
        double v_cur = _uv_coords[2 * heh.idx() + 1];
        ++vih_it;
        int n_boundaries(0);
        for (; vih_it.is_valid(); ++vih_it) {
            const HEH heh_cur = *vih_it;
            if (!tri_mesh_.is_boundary(heh_cur)) {
                // apply transition to next triangle
                HEH heh_opp = tri_mesh_.opposite_halfedge_handle(*vih_it);
                transition(heh_opp).transform_point(u_cur, v_cur);
                // store updated values
                _uv_coords[2 * heh_cur.idx()] = u_cur;
                _uv_coords[2 * heh_cur.idx() + 1] = v_cur;
            } else
                ++n_boundaries;
        }

        if (n_boundaries > 1)
            std::cerr << "ERROR in Mesh Extraction: input triangle mesh has non-manifold vertex"
                << " which is adjacent to more than one boundary!" << std::endl;
    }
}

template<class TMeshT>
std::string MeshExtractorT<TMeshT>::getParametrizationStats(std::vector<double>& _uv_coords) {
    extract_transition_functions(_uv_coords);
    consistent_truncation(_uv_coords);

    int faces_needle = 0;
    int faces_cap = 0;
    int faces_degen_point = 0;
    int faces_positive = 0;
    int faces_negative = 0;

    for (FIter f_it = tri_mesh_.faces_sbegin(), f_end = tri_mesh_.faces_end(); f_it != f_end; ++f_it) {
        const FH fh = *f_it;

        // get three halfedge_handles of triangle
        const HEH heh0 = tri_mesh_.halfedge_handle(fh);
        const HEH heh1 = tri_mesh_.next_halfedge_handle(heh0);
        const HEH heh2 = tri_mesh_.next_halfedge_handle(heh1);

        // get three uv-positions of vertices
        const Point_2 p0(_uv_coords[2 * heh0.idx()], _uv_coords[2 * heh0.idx() + 1]);
        const Point_2 p1(_uv_coords[2 * heh1.idx()], _uv_coords[2 * heh1.idx() + 1]);
        const Point_2 p2(_uv_coords[2 * heh2.idx()], _uv_coords[2 * heh2.idx() + 1]);

        // construct triangle
        const Triangle_2 tri(p0, p1, p2);

        const ORIENTATION ori = tri.orientation();

        if (ori != ORI_ZERO) {
            if (ori == ORI_POSITIVE)
                ++faces_positive;
            else
                ++faces_negative;
            continue;
        }

        if (p0 == p1 && p1 == p2)
            ++faces_degen_point;
        else if (p0 == p1 || p1 == p2 || p2 == p0)
            ++faces_needle;
        else
            ++faces_cap;
    }

    std::ostringstream stats;
    stats << "Parametrization stats:" << std::endl
            << "  # positive: " << faces_positive << std::endl
            << "  # negative: " << faces_negative << std::endl
            << "  # needles: " << faces_needle << std::endl
            << "  # caps: " << faces_cap << std::endl
            << "  # points: " << faces_degen_point << std::endl;
    std::cout << stats.str();

    return stats.str();
}

template<class TMeshT>
template<typename EMBEDDING>
void MeshExtractorT<TMeshT>::generate_vertices(
        std::vector<double>& _uv_coords,
        const std::vector<unsigned int> * const _external_valences,
        EMBEDDING embedding) {

    tri_mesh_.request_face_colors();

    // --------------------------------------------------------
    // 1. Generate unique assignment
    // --------------------------------------------------------
    vertex_to_halfedge_.resize(tri_mesh_.n_vertices());
    edge_to_halfedge_.resize(tri_mesh_.n_edges());

    for (VIter v_it = tri_mesh_.vertices_sbegin(), v_end = tri_mesh_.vertices_end(); v_it != v_end; ++v_it) {
        const VIHIter vih_iter = tri_mesh_.vih_iter(*v_it);
        if (vih_iter.is_valid())
            vertex_to_halfedge_[v_it->idx()] = *vih_iter;
        else
            vertex_to_halfedge_[v_it->idx()] = HEH(-1);
    }

    for (EIter e_it = tri_mesh_.edges_sbegin(), e_end = tri_mesh_.edges_end(); e_it != e_end; ++e_it) {
        const EH eh = *e_it;
        const HEH heh0 = tri_mesh_.halfedge_handle(eh, 0);
        const HEH heh1 = tri_mesh_.halfedge_handle(eh, 1);

        if (!tri_mesh_.is_boundary(heh0))
            edge_to_halfedge_[eh.idx()] = heh0;
        else
            edge_to_halfedge_[eh.idx()] = heh1;
    }

    // --------------------------------------------------------
    // 2. Traverse Faces and generate grid vertices
    // --------------------------------------------------------

    /*
     * Generate face q-vertices.
     */
    // clear old data
    gvertices_.clear();
    // Skip the first 15 reallocations.
    gvertices_.reserve(32768);
    // clear and resize container
    face_gvertices_.clear();
    face_gvertices_.resize(tri_mesh_.n_faces());
    int n_face_gvertices(0);

    // extract vertices within faces
    for (FIter f_it = tri_mesh_.faces_sbegin(), f_end = tri_mesh_.faces_end(); f_it != f_end; ++f_it) {
        const FH fh = *f_it;
        // get three halfedge_handles of triangle
        const HEH heh0 = tri_mesh_.halfedge_handle(fh);
        const HEH heh1 = tri_mesh_.next_halfedge_handle(heh0);
        const HEH heh2 = tri_mesh_.next_halfedge_handle(heh1);

        // get three uv-positions of vertices
        const Point_2 p0(_uv_coords[2 * heh0.idx()], _uv_coords[2 * heh0.idx() + 1]);
        const Point_2 p1(_uv_coords[2 * heh1.idx()], _uv_coords[2 * heh1.idx() + 1]);
        const Point_2 p2(_uv_coords[2 * heh2.idx()], _uv_coords[2 * heh2.idx() + 1]);

        // construct triangle
        const Triangle_2 tri(p0, p1, p2);
        const ORIENTATION tri_orientation = tri.orientation();

        // non-degenerate?
        if (tri_orientation != ORI_ZERO) {
            tri_mesh_.set_color(fh, typename TMeshT::Color(1.0, 1.0, 1.0, 1.0));

            // get mapping between 2d and 3d
            const Point pp0 = embedding(heh0);
            const Point pp1 = embedding(heh1);
            const Point pp2 = embedding(heh2);
            const Matrix_3 M = get_mapping(tri, pp0, pp1, pp2);

            const Bbox_2 bb = tri.bbox();

            const int x_min = std::ceil(bb.xmin());
            const int x_max = std::floor(bb.xmax());
            const int y_min = std::ceil(bb.ymin());
            const int y_max = std::floor(bb.ymax());

            for (int x = x_min; x <= x_max; ++x) {
                for (int y = y_min; y <= y_max; ++y) {
                    if (tri.has_on_bounded_side(Point_2(x, y))) {
                        // point should be strictly inside the triangle
                        assert(!tri.has_on_boundary(Point_2(x, y)));

                        const Point p3d = applyMapping(M, x, y);

                        // store reference
                        face_gvertices_[fh.idx()].push_back(gvertices_.size());
                        // store new GridVertex
                        gvertices_.push_back(GridVertex(GridVertex::OnFace, heh0, Point_2(x, y), p3d, false));
                        // construct local edge information
                        construct_local_edge_information_face(gvertices_.back(), _uv_coords);
                        ++n_face_gvertices;
                    }
                }
            }
        } else {
            tri_mesh_.set_color(fh, typename TMeshT::Color(1.0, 0.0, 0.0, 1.0));
        }
    }

    /*
     * Generate edge q-vertices
     */
    // clear and resize container
    edge_valid_.resize(tri_mesh_.n_edges());
    edge_gvertices_.clear();
    edge_gvertices_.resize(tri_mesh_.n_edges());
    int n_edge_gvertices(0);

    // extract vertices within edges
    for (EIter e_it = tri_mesh_.edges_sbegin(), e_end = tri_mesh_.edges_end(); e_it != e_end; ++e_it) {
        const EH eh = *e_it;
        // get corresponding face
        if (!(edge_to_halfedge_[eh.idx()].is_valid())) {
            std::cerr << "Warning: edge does not have a valid halfedge... " << eh.idx() << " - "
                    << edge_to_halfedge_[eh.idx()].idx() << std::endl;
            continue;
        }

        // get two halfedges to vertices of edge
        const HEH heh0 = edge_to_halfedge_[eh.idx()];
        const HEH heh1 = tri_mesh_.prev_halfedge_handle(heh0);

        // get two uv-positions of vertices
        const Point_2 p0(_uv_coords[2 * heh0.idx()], _uv_coords[2 * heh0.idx() + 1]);
        const Point_2 p1(_uv_coords[2 * heh1.idx()], _uv_coords[2 * heh1.idx() + 1]);

        // construct Edge Segment
        const Segment_2 seg(p0, p1);

        // positive orientation and non-degenerate?
        if (!seg.is_degenerate()) {
            edge_valid_[eh.idx()] = true;

            // get mapping between 2d and 3d
            const Point pp0 = embedding(heh0);
            const Point pp1 = embedding(heh1);

            const Matrix_3 M = get_mapping(seg, pp0, pp1);

            const Bbox_2 bb = seg.bbox();

            int x_min = std::ceil(bb.xmin());
            int x_max = std::floor(bb.xmax());
            int y_min = std::ceil(bb.ymin());
            int y_max = std::floor(bb.ymax());

            // x-range larger?
            if (bb.xmax() - bb.xmin() >= bb.ymax() - bb.ymin()) {
                // remove boundary points
                if (double(x_min) == bb.xmin())
                    ++x_min;
                if (double(x_max) == bb.xmax())
                    --x_max;

                // iterate over x and compute closest y
                for (int x = x_min; x <= x_max; ++x) {

                    // compute y candidate
                    const double alpha = (x - p0[0]) / (p1[0] - p0[0]);
                    const int y = ROUND_QME(p0[1] + alpha*(p1[1]-p0[1]));

                    // valid?
                    if (y >= y_min && y <= y_max) {
                        if (seg.has_on(Point_2(x, y))) {
                            Point p3d(0, 0, 0);
                            for (unsigned int i = 0; i < 3; ++i)
                                p3d[i] += M(i, 0) * x + M(i, 1) * y + M(i, 2);

                            // store reference
                            edge_gvertices_[eh.idx()].push_back(gvertices_.size());
                            // store new GridVertex
                            gvertices_.push_back(GridVertex(GridVertex::OnEdge, heh0, Point_2(x, y), p3d, false));
                            // construct local edge information
                            construct_local_edge_information_edge(gvertices_.back(), _uv_coords);
                            ++n_edge_gvertices;
                        }
                    }
                }
            } else {
                // remove boundary points
                if (double(y_min) == bb.ymin())
                    ++y_min;
                if (double(y_max) == bb.ymax())
                    --y_max;

                // iterate over y and compute closest x
                for (int y = y_min; y <= y_max; ++y) {
                    // compute y candidate
                    double alpha = (y - p0[1]) / (p1[1] - p0[1]);
                    int x = ROUND_QME(p0[0] + alpha*(p1[0]-p0[0]));

                    // valid?
                    if (x >= x_min && x <= x_max)
                        if (seg.has_on(Point_2(x, y))) {
                            Point p3d(0, 0, 0);
                            for (unsigned int i = 0; i < 3; ++i)
                                p3d[i] += M(i, 0) * x + M(i, 1) * y + M(i, 2);

                            // store reference
                            edge_gvertices_[eh.idx()].push_back(gvertices_.size());
                            // store new GridVertex
                            gvertices_.push_back(
                                    GridVertex(GridVertex::OnEdge, heh0, Point_2(x, y), p3d, false));
                            // construct local edge information
                            construct_local_edge_information_edge(gvertices_.back(), _uv_coords);
                            ++n_edge_gvertices;
                        }
                }
            }

        } else
            edge_valid_[eh.idx()] = false;
    }

    /*
     * Generate vertex q-vertices.
     */
    // clear and resize container
    vertex_gvertices_.clear();
    vertex_gvertices_.resize(tri_mesh_.n_vertices());
    int n_vertex_gvertices(0);

    // extract vertices within faces
    for (VIter v_it = tri_mesh_.vertices_sbegin(), v_end = tri_mesh_.vertices_end(); v_it != v_end; ++v_it) {
        const VH vh = *v_it;
        // get corresponding halfedge
        const HEH heh = vertex_to_halfedge_[vh.idx()];
        if (!heh.is_valid())
            continue;

        // get uv-position of vertex
        const Point_2 p(_uv_coords[2 * heh.idx()], _uv_coords[2 * heh.idx() + 1]);

        // vertex at integer location?
        if (p[0] == ROUND_QME(p[0]) && p[1] == ROUND_QME(p[1])) {
            // store reference
            vertex_gvertices_[vh.idx()].push_back(gvertices_.size());
            // store new GridVertex
            gvertices_.push_back(
                    GridVertex(GridVertex::OnVertex, heh, p,
                               embedding(tri_mesh_.opposite_halfedge_handle(tri_mesh_.halfedge_handle(vh))), false));
            // construct local edge information
            construct_local_edge_information_vertex(gvertices_.back(), _uv_coords, _external_valences);

            ++n_vertex_gvertices;
        }
    }
}

template<class TMeshT>
ORIENTATION
MeshExtractorT<TMeshT>::
triangleUvOrientation(FH fh, const std::vector<double> &uv_coords) {
    const HEH heh0 = tri_mesh_.halfedge_handle(fh);
    const HEH heh1 = tri_mesh_.next_halfedge_handle(heh0);
    const HEH heh2 = tri_mesh_.next_halfedge_handle(heh1);

    // get three uv-positions of vertices
    const Point_2 p0(uv_coords[2*heh0.idx()], uv_coords[2*heh0.idx()+1]);
    const Point_2 p1(uv_coords[2*heh1.idx()], uv_coords[2*heh1.idx()+1]);
    const Point_2 p2(uv_coords[2*heh2.idx()], uv_coords[2*heh2.idx()+1]);

    // construct triangle
    const Triangle_2 tri(p0, p1, p2);

    return tri.orientation();
}

template<class TMeshT>
void MeshExtractorT<TMeshT>::construct_local_edge_information_face(GridVertex& _gv, const std::vector<double>& _uv_coords) {

    // clear old data
    _gv.local_edges.clear();
    _gv.local_edges.reserve(4);

    if (_gv.heh.is_valid() && !tri_mesh_.is_boundary(_gv.heh)) {

        const FH fh = tri_mesh_.face_handle(_gv.heh);
        Point_2 uv = _gv.position_uv;

        // convention is to start with x-dir and then rotate by k*90
        // this is CCW!!!
        typedef std::vector<Vector_2> V2V;
        for (V2V::iterator direction_it = cartesian_orientations_.begin(), it_end = cartesian_orientations_.end();
                direction_it != it_end; ++direction_it)
            _gv.local_edges.push_back(LocalEdgeInfo(fh, uv, uv + *direction_it));

        /*
         * Outgoing edges ordering should be consistent
         * with face orientation.
         */
        if (triangleUvOrientation(fh, _uv_coords) == ORI_NEGATIVE)
            std::reverse(_gv.local_edges.begin(), _gv.local_edges.end());
    }
}


template<class TMeshT>
void MeshExtractorT<TMeshT>::construct_local_edge_information_edge(GridVertex& _gv, const std::vector<double>& _uv_coords) {
    // clear old data
    _gv.local_edges.clear();
    _gv.local_edges.reserve(4);

    if (_gv.heh.is_valid() && !tri_mesh_.is_boundary(_gv.heh)) {
        const HEH heh = _gv.heh;
        const HEH heh_opp = tri_mesh_.opposite_halfedge_handle(heh);

        // is boundary ? -> tag as boundary vertex
        if (tri_mesh_.is_boundary(tri_mesh_.edge_handle(heh)))
            _gv.is_boundary = true;

        const FH fh = tri_mesh_.face_handle(heh);
        const ORIENTATION ori = triangleUvOrientation(fh, _uv_coords);

        FH fh_opp(-1);
        ORIENTATION ori_opp = ORI_ZERO;
        if (!tri_mesh_.is_boundary(heh_opp)) {
            fh_opp = tri_mesh_.face_handle(heh_opp);
            ori_opp = triangleUvOrientation(fh_opp, _uv_coords);
        }

        // get uv-coords
        const Point_2 uv = _gv.position_uv;

        // get uv-coords in opposite face
        const TF tf = transition(heh);
        Point_2 uv_opp(uv);
        tf.transform_point(uv_opp);

        typedef std::vector<Vector_2> V2V;
        /*
         * Add directions of face one.
         */
        {
            // get edge segment
            const HEH heh_prev = tri_mesh_.prev_halfedge_handle(heh);
            const Point_2 p1(_uv_coords[2 * heh.idx()], _uv_coords[2 * heh.idx() + 1]);
            const Point_2 p0(_uv_coords[2 * heh_prev.idx()], _uv_coords[2 * heh_prev.idx() + 1]);

            size_t middleEl = 0;
            for (V2V::iterator direction_it = cartesian_orientations_.begin(), it_end = cartesian_orientations_.end(); direction_it != it_end; ++direction_it) {

                const Point_2 to_uv = uv + *direction_it;
                const ORIENTATION path_ori = Triangle_2(p0, p1, to_uv).orientation();

                if (path_ori == ori) {
                    _gv.local_edges.push_back(LocalEdgeInfo(fh, uv, to_uv));
                } else if (path_ori == ORI_ZERO) {
                    if (direction_it->dot(p1 - p0) > 0 || !fh_opp.is_valid()) {
                        _gv.local_edges.push_back(LocalEdgeInfo(fh, uv, to_uv));
                    } else {
                        middleEl = _gv.local_edges.size();
                    }
                } else {
                    middleEl = _gv.local_edges.size();
                }
            }
            /*
             * If the sequence of directions was interrupted, reorder it so that all
             * valid directions are in sequence.
             */
            if (middleEl && middleEl < _gv.local_edges.size())
                std::rotate(_gv.local_edges.begin(), _gv.local_edges.begin() + middleEl, _gv.local_edges.end());

            /*
             * Outgoing edges ordering should be consistent
             * with face orientation.
             */
            if (ori == ORI_NEGATIVE)
                std::reverse(_gv.local_edges.begin(), _gv.local_edges.end());
        }
        /*
         * Add directions of face two.
         */
        if (fh_opp.is_valid()) {
            // get edge segment
            const HEH heh_prev = tri_mesh_.prev_halfedge_handle(heh_opp);
            const Point_2 p1(_uv_coords[2 * heh_opp.idx()], _uv_coords[2 * heh_opp.idx() + 1]);
            const Point_2 p0(_uv_coords[2 * heh_prev.idx()], _uv_coords[2 * heh_prev.idx() + 1]);

            const TF tf = transition(heh);

            const size_t le_ofs = _gv.local_edges.size();

            size_t middleEl = 0;
            for (V2V::iterator direction_it = cartesian_orientations_.begin(), it_end = cartesian_orientations_.end(); direction_it != it_end; ++direction_it) {

                Point_2 to_uv = uv + *direction_it;
                tf.transform_point(to_uv);
                const Vector_2 trans_direction = to_uv - uv_opp;

                const ORIENTATION path_ori = Triangle_2(p0, p1, to_uv).orientation();
                if (path_ori == ori_opp
                        || (path_ori == ORI_ZERO && trans_direction.dot(p1 - p0) > 0)) {
                    _gv.local_edges.push_back(LocalEdgeInfo(fh_opp, uv_opp, to_uv));
                } else {
                    middleEl = _gv.local_edges.size();
                }
            }

            /*
             * If the sequence of directions was interrupted, reorder it so that all
             * valid directions are in sequence.
             */
            if (middleEl > le_ofs && middleEl < _gv.local_edges.size())
                std::rotate(_gv.local_edges.begin() + le_ofs, _gv.local_edges.begin() + middleEl,
                        _gv.local_edges.end());

            /*
             * Outgoing edges ordering should be consistent
             * with face orientation.
             */
            if (ori_opp == ORI_NEGATIVE)
                std::reverse(_gv.local_edges.begin() + le_ofs, _gv.local_edges.end());
        }
    }
}


//-----------------------------------------------------------------------------

template<class TMeshT>
void
MeshExtractorT<TMeshT>::
construct_local_edge_information_vertex(GridVertex& _gv, const std::vector<double>& _uv_coords, const std::vector<unsigned int> * const _external_valences)
{
  // clear old data
  _gv.local_edges.clear(); _gv.local_edges.reserve(4);

  if( _gv.heh.is_valid() && !tri_mesh_.is_boundary(_gv.heh))
  {
    VH  vh  = tri_mesh_.to_vertex_handle(_gv.heh);

    // is boundary ? -> set tag
    if(tri_mesh_.is_boundary(vh))
      _gv.is_boundary = true;

    /*
     * Traverse incoming halfedges in CCW order.
     */
    double initial_neg_angleSum = 0, pos_angleSum = 0, neg_angleSum = 0;
    for(VIHIter vih_it = tri_mesh_.vih_iter(vh); vih_it.is_valid(); --vih_it) {
      if(!tri_mesh_.is_boundary(*vih_it))
      {
        // get opposite edge points
        const HEH heh  = *vih_it;
        const HEH heh1 = tri_mesh_.next_halfedge_handle(heh );
        const HEH heh2 = tri_mesh_.next_halfedge_handle(heh1);
        const Point_2 uv0(_uv_coords[2*heh .idx()], _uv_coords[2*heh .idx()+1]);
        const Point_2 uv1(_uv_coords[2*heh1.idx()], _uv_coords[2*heh1.idx()+1]);
        const Point_2 uv2(_uv_coords[2*heh2.idx()], _uv_coords[2*heh2.idx()+1]);

        const Vector_2 sector_left = uv2 - uv0;
        const Vector_2 sector_right = uv1 - uv0;
        const ORIENTATION orientation = Triangle_2(uv0, uv1, uv2).orientation();

        if (orientation == ORI_CCW) { // positive triangle
            if (neg_angleSum > 0) { // this marks the end of a negative triangle fan
                pos_angleSum += 2 * M_PI - neg_angleSum;
                neg_angleSum = 0;
            }
            const double angle = std::acos((sector_left.dot(sector_right)) / (sector_left.norm() * sector_right.norm()));
            pos_angleSum += angle;

        } else if (orientation == ORI_CW) { // negative triangle
            const double angle = std::acos((sector_left.dot(sector_right)) / (sector_left.norm() * sector_right.norm()));
            if (pos_angleSum == 0) {
                initial_neg_angleSum += angle;
            } else {
                neg_angleSum += angle;
            }
        }

        // is opposite halfedge boundary?
        bool is_left_opp_boundary = tri_mesh_.is_boundary(tri_mesh_.opposite_halfedge_handle(heh));

        FH fh = tri_mesh_.face_handle(heh);

        // LocalEdgeInfo per face
        std::vector<LocalEdgeInfo> leis_per_face;

        size_t middleEl = 0;
        // test for outgoing edges
        for(unsigned int i=0; i<cartesian_orientations_.size(); ++i)
        {
          const ORIENTATION ori1 = orient2d( sector_right, cartesian_orientations_[i]);
          const ORIENTATION ori2 = orient2d( cartesian_orientations_[i], sector_left);

          if (is_left_opp_boundary && ori2 == ORI_COLLINEAR && cartesian_orientations_[i].dot(uv2-uv0) > 0) {

              // On left edge and no face to the left
              leis_per_face.push_back(LocalEdgeInfo(fh, uv0, uv0+cartesian_orientations_[i]));
          } else if (ori1 == ORI_COLLINEAR && (uv1-uv0).dot(cartesian_orientations_[i]) > 0) {

              // On right edge
              leis_per_face.push_back(LocalEdgeInfo(fh, uv0, uv0+cartesian_orientations_[i]));
          } else if (ori1 == orientation && ori2 == orientation) {

              // Inside triangle
              leis_per_face.push_back(LocalEdgeInfo(fh, uv0, uv0+cartesian_orientations_[i]));
          } else {
              middleEl = leis_per_face.size();
          }
        }

        if (middleEl && middleEl < leis_per_face.size())
            std::rotate(leis_per_face.begin(), leis_per_face.begin() + middleEl, leis_per_face.end());

        /*
         * Outgoing edges ordering should be consistent
         * with face orientation.
         */
        if (orientation == ORI_NEGATIVE)
            std::reverse(leis_per_face.begin(), leis_per_face.end());
        std::copy(leis_per_face.begin(), leis_per_face.end(), std::back_inserter(_gv.local_edges));
      }
    }

    if (initial_neg_angleSum > 0 || neg_angleSum > 0) {
        neg_angleSum += initial_neg_angleSum;
        pos_angleSum += 2 * M_PI - neg_angleSum;
    }

    const double ninetyJump = pos_angleSum / M_PI_2;
    /*
     * Assertion removed: Determining the angle is inexact. If the parameter triangles are
     * almost degenerate the angle can be arbitrarily far off. That's why we can't really
     * count on the number of expected LEIs we determine here.
     */
    // assert(_gv.is_boundary || fabs(ninetyJump - round(ninetyJump)) < 1e-6);

    const int expected_lei_count =
            _external_valences ? (*_external_valences)[vh.idx()] : static_cast<unsigned int>(ROUND_QME(ninetyJump));
    _gv.missing_leis = expected_lei_count - static_cast<int>(_gv.local_edges.size());
    /*
     * This heuristic doesn't work for boundary vertices (due to
     * the inexact arithmetic used here). It also fails if triangles are
     * degenerate or almost degenerate.
     */
    if (_gv.is_boundary) _gv.missing_leis = 0;
  }
}


//-----------------------------------------------------------------------------

template<class TMeshT>
void
MeshExtractorT<TMeshT>::
increment_opposite_connected_to_idx(typename std::vector<LocalEdgeInfo>::iterator first, typename std::vector<LocalEdgeInfo>::iterator last) {
    for (; first != last; ++first) {
        if (first->connected_to_idx >= LocalEdgeInfo::LECI_Connected_Thresh)
            gvertices_[first->connected_to_idx]
                       .local_edge(first->orientation_idx).orientation_idx += 1;
    }
}

template<class TMeshT>
bool
MeshExtractorT<TMeshT>::
notConnected(GridVertex &gv1, GridVertex &gv2) {
    for (typename std::vector<LocalEdgeInfo>::const_iterator it = gv1.local_edges.begin(), it_end = gv1.local_edges.end(); it != it_end; ++it) {
        if (it->connected_to_idx >= LocalEdgeInfo::LECI_Connected_Thresh &&
                &gvertices_[it->connected_to_idx] == &gv2) return false;
    }
    return true;
}

template<class TMeshT>
void
MeshExtractorT<TMeshT>::
try_connect_incomplete_gvertices() {
    /*
     * For each gvertex:
     *   If missing_leis > 0:
     *     For each LEI:
     *       Trace face, track local UVs (same strategy as in generate_faces_and_store_quadmesh)
     *       Upon encounter of a vertex with same UV as pivot:
     *         Insert LEI in both vertices at appropriate position. Connect them.
     */

    size_t start_gv_idx = 0;
    for (typename std::vector<GridVertex>::iterator start_gv_it = gvertices_.begin(), gv_end = gvertices_.end();
            start_gv_it != gv_end; ++start_gv_it, ++start_gv_idx) {
        if (start_gv_it->missing_leis == 0) continue;

        /*
         * Warning: start_gv_it->local_edges.end() is not cached on purpose because we
         * add elements to local_edges in the loop body, i.e. local_edges.end() may change
         * while iterating.
         */
        for (typename std::vector<LocalEdgeInfo>::iterator start_lei_it = start_gv_it->local_edges.begin();
                start_lei_it != start_gv_it->local_edges.end(); ++start_lei_it) {

            typename std::vector<LocalEdgeInfo>::iterator start_gv_lei_insert_before = start_lei_it + 1;
            LocalEdgeInfo * const final_lei = &start_gv_it->local_edge(std::distance(start_gv_it->local_edges.begin(), start_lei_it) + 1);

            /*
             * Trace face starting with *start_lei_it.
             */
            GridVertex *current_from_gv = &*start_gv_it;
            LocalEdgeInfo *current_outgoing_lei = &*start_lei_it;

            TF accumulated_tf = TF::IDENTITY;
            const Point_2 pivot_uv = current_outgoing_lei->uv_from;//current_from_gv->position_uv;

            bool edgeCreated = false;

            do {

                if (!current_outgoing_lei->isConnected()) break;

                GridVertex * const next_gv = &gvertices_[current_outgoing_lei->connected_to_idx];
                const int next_incoming_lei_idx = current_outgoing_lei->orientation_idx;
                LocalEdgeInfo * next_incoming_lei = &next_gv->local_edge(next_incoming_lei_idx);
                LocalEdgeInfo * next_outgoing_lei = &next_gv->local_edge(next_incoming_lei_idx - 1);

                /*
                 * Apply local edge portion to the accumulated transition function.
                 * The intra face portion is applied after the edge insertion.
                 */
                accumulated_tf = current_outgoing_lei->accumulated_tf * accumulated_tf;
                const TF intra_face_tf =
                        intra_gv_transition(next_incoming_lei->fh_from, next_outgoing_lei->fh_from, *next_gv, next_incoming_lei != next_outgoing_lei)
                        * intra_gv_transition(next_incoming_lei->fh_from, tri_mesh_.face_handle(next_gv->heh), *next_gv, true).inverse();


                /*
                 * Compute next gv's UV transformed into original coordinate system.
                 */
                Point_2 next_uv = next_outgoing_lei->uv_from;
                (intra_face_tf * accumulated_tf).inverse().transform_point(next_uv);

                /*
                 * Conditionally connect next_gv with *start_gv_it.
                 */
                if (!edgeCreated && next_uv == pivot_uv && next_gv != &*start_gv_it && notConnected(*next_gv, *start_gv_it)) {

#if 1
                    /*
                     * Find iterator of next_outgoing_lei.
                     */
                    typename std::vector<LocalEdgeInfo>::iterator next_incoming_lei_it =
                            std::find_if(next_gv->local_edges.begin(), next_gv->local_edges.end(), LEIPointerEquality<TMeshT>(*next_incoming_lei));
                    assert(next_incoming_lei_it != next_gv->local_edges.end());

                    /*
                     * Increment opposite LEI's connected_to_idx for LEIs after insertion position.
                     */
                    increment_opposite_connected_to_idx(start_gv_lei_insert_before, start_gv_it->local_edges.end());
                    increment_opposite_connected_to_idx(next_incoming_lei_it, next_gv->local_edges.end());
                    const TF new_incoming_lei_tf =
                            (
                                    intra_gv_transition(final_lei->fh_from, start_lei_it->fh_from, *start_gv_it, true) *
                                    intra_gv_transition(final_lei->fh_from, start_lei_it->fh_from, *start_gv_it, final_lei != &*start_lei_it).inverse() *
                                    accumulated_tf.inverse()
                            ).inverse();
                    const TF new_outgoing_lei_tf =
                            (
                                intra_gv_transition(next_incoming_lei->fh_from, tri_mesh_.face_handle(next_gv->heh), *next_gv, true).inverse() *
                                accumulated_tf *
                                intra_gv_transition(start_lei_it->fh_from, tri_mesh_.face_handle(start_gv_it->heh), *start_gv_it, true).inverse()
                            ).inverse();

                    typename std::vector<LocalEdgeInfo>::iterator new_incoming_lei_it =
                            start_gv_it->local_edges.insert(start_gv_lei_insert_before,
                                                            LocalEdgeInfo(start_lei_it->fh_from, start_lei_it->uv_from, start_lei_it->uv_from));
                    /*
                     *  *** start_lei_it invalid starting here! ***
                     */
                    typename std::vector<LocalEdgeInfo>::iterator new_outgoing_lei_it =
                            next_gv->local_edges.insert(next_incoming_lei_it,
                                                        LocalEdgeInfo(next_incoming_lei->fh_from, next_incoming_lei->uv_from, next_incoming_lei->uv_from));

                    /*
                     * Correct iterator.
                     */
                    start_lei_it = new_incoming_lei_it - 1;

                    /*
                     * Correct pointers. (Follow newly created connection.)
                     */
                    next_incoming_lei = &next_gv->local_edge(next_incoming_lei_idx + 1);
                    next_outgoing_lei = &next_gv->local_edge(next_incoming_lei_idx);
                    assert(&next_gv->local_edge(next_incoming_lei_idx) == &*new_outgoing_lei_it);

                    new_incoming_lei_it->completeInformation(current_outgoing_lei->connected_to_idx,
                                                             std::distance(next_gv->local_edges.begin(), new_outgoing_lei_it),
                                                             start_lei_it->uv_from, new_incoming_lei_tf);
                    new_outgoing_lei_it->completeInformation(start_gv_idx,
                                                             std::distance(start_gv_it->local_edges.begin(), new_incoming_lei_it),
                                                             next_incoming_lei->uv_from, new_outgoing_lei_tf);

                    edgeCreated = true;
#endif
                }

                /*
                 * Apply intra face portion of the transition function.
                 */
                accumulated_tf = intra_face_tf * accumulated_tf;

                /*
                 * Advance
                 */
                current_from_gv = next_gv;
                current_outgoing_lei = next_outgoing_lei;

            } while (current_from_gv != &*start_gv_it);
            // Break when back at original grid vertex.
        }
    }
}

template<class TMeshT>
void
MeshExtractorT<TMeshT>::
generate_connections(std::vector<double>& _uv_coords)
{
  for(unsigned int i=0; i<gvertices_.size(); ++i) {
    for(unsigned int j=0; j<gvertices_[i].local_edges.size(); ++j)
      // unconnected ?
      if(gvertices_[i].local_edges[j].isUnconnected() && gvertices_[i].local_edges[j].fh_from.is_valid())
      {
        // find path via tracing
        FindPathResult target = find_path(gvertices_[i], gvertices_[i].local_edges[j], _uv_coords);

                // store result
        target.applyToLocalEdgeInfo(gvertices_[i].local_edges[j]);

        // boundary?
        if(target.connected_to_idx == LocalEdgeInfo::LECI_Traced_Into_Boundary)
          gvertices_[i].is_boundary = true;

        // found partner? -> store reverse
        if(target.connected_to_idx >= LocalEdgeInfo::LECI_Connected_Thresh)
        {
          assert(target.connected_to_idx >= 0 && (size_t)target.connected_to_idx < gvertices_.size());
          assert(target.orientation_idx >= 0 && (size_t)target.orientation_idx < gvertices_[target.connected_to_idx].local_edges.size());
          // not yet connected?
          if(gvertices_[target.connected_to_idx].local_edges[target.orientation_idx].isUnconnectedOrSignal()) {
              //assert(gvertices_[target.connected_to_idx].local_edges[target.orientation_idx].accumulated_tf == TF::IDENTITY);
              assert(gvertices_[i].local_edges[j].accumulated_tf == target.accumulated_tf);
              TF reverse_tf =
                      intra_gv_transition(gvertices_[target.connected_to_idx].local_edges[target.orientation_idx].fh_from,
                                          tri_mesh_.face_handle(gvertices_[target.connected_to_idx].heh),
                                          gvertices_[target.connected_to_idx], true).inverse() *
                      gvertices_[i].local_edges[j].accumulated_tf *
                      intra_gv_transition(gvertices_[i].local_edges[j].fh_from,
                                          tri_mesh_.face_handle(gvertices_[i].heh),
                                          gvertices_[i], true).inverse()
              ;
              /*
               * Translate opposite lei's destination UVs into coordinate system of origin of opposite lei.
               */
              Point_2 opposite_to = gvertices_[i].position_uv; // Coordinate system (CS) of GV_i
              reverse_tf.transform_point(opposite_to); // reverse_tf maps from CS of GV_i -> CS of lei.fh_from -> CS of GV_{target} -> opposite_lei.fh_from.
              reverse_tf = reverse_tf.inverse();

             gvertices_[target.connected_to_idx].local_edges[target.orientation_idx].completeInformation(i,j, opposite_to,
                                                                                                         //TF::IDENTITY
                                                                                                         reverse_tf
                                                                                                         //target.accumulated_tf.inverse()
                                                                                                         );
          } else
          {
              const GridVertex &peer = gvertices_[target.connected_to_idx];
              std::cerr << "\x1b[41mWarning: When tracing from GV " << i
                      << ", LEI " << j << " I hit GV " << target.connected_to_idx
                      << ", LEI " << target.orientation_idx << "." << std::endl
                      << "  However, this GV is already connected to GV "
                      << peer.local_edges[target.orientation_idx].connected_to_idx
                      << ", LEI "
                      << peer.local_edges[target.orientation_idx].orientation_idx
                      << "\x1b[0m" << std::endl;
              // remove found connection
              gvertices_[i].local_edges[j].connected_to_idx = LocalEdgeInfo::LECI_No_Connection;
          }
        }
      }
  }
}


template<class TMeshT>
typename MeshExtractorT<TMeshT>::FindPathResult
MeshExtractorT<TMeshT>::
find_path(const GridVertex& _gv, const LocalEdgeInfo& lei, std::vector<double>& _uv_coords)
{
  // get current data
  FH cur_fh = lei.fh_from;
  Point_2  uv_from = lei.uv_from;
  Point_2  uv_original_from = lei.uv_from;
  Point_2  uv_to   = lei.uv_intended_to;

  // initialize first halfedge
  HEH cur_heh(-1);

  // get halfedges of triangle
  HEH heh0 = tri_mesh_.halfedge_handle(cur_fh);
  HEH heh1 = tri_mesh_.next_halfedge_handle(heh0);
  HEH heh2 = tri_mesh_.next_halfedge_handle(heh1);

  // get points
  Point_2 uv0(_uv_coords[2*heh0.idx()], _uv_coords[2*heh0.idx()+1]);
  Point_2 uv1(_uv_coords[2*heh1.idx()], _uv_coords[2*heh1.idx()+1]);
  Point_2 uv2(_uv_coords[2*heh2.idx()], _uv_coords[2*heh2.idx()+1]);

  // get triangle
  Triangle_2 tri(uv0, uv1, uv2);

  bool inverted = (tri.orientation() == ORI_NEGATIVE);

  TF accumulated_tf = TF::IDENTITY;
  // start and endpoint in same face? -> cheap out
  BOUNDEDNESS bs = tri.boundedness(uv_to);
  if(bs == BND_ON_BOUNDED_SIDE || bs == BND_ON_BOUNDARY)
  {
    return find_local_connection(uv_from, uv_original_from, uv_to, tri, heh0, heh1, heh2, bs, accumulated_tf, _uv_coords);
  }
  else // endpoint not within triangle -> do first step
  {
    // get path segment
    Segment_2 path(uv_from, uv_to);

    switch(_gv.type)
    {
    // ############# CASE OnFace ###############
      case GridVertex::OnFace:
      {
        /*
         *  Intersect outgoing quad edge with the
         *  triangle edges.
         */
        if (path.intersects(Segment_2(uv2, uv0)))
            cur_heh = heh0;
        else if (path.intersects(Segment_2(uv0, uv1)))
            cur_heh = heh1;
        else if (path.intersects(Segment_2(uv1, uv2)))
            cur_heh = heh2;
        else {
            std::cerr << "Warning: find_path, type OnFace with endpoint outside triangle must intersect one edge segment!!!" << std::endl;
            return FindPathResult::Error();
        }

      }break;

      // ############# CASE OnEdge ###############
      case GridVertex::OnEdge:
      {
        // get halfedge of edge
        cur_heh = _gv.heh;

        // go to opposite?
        if(tri_mesh_.is_boundary(cur_heh) || tri_mesh_.face_handle(cur_heh) != cur_fh)
          cur_heh = tri_mesh_.opposite_halfedge_handle(cur_heh);

        assert( tri_mesh_.face_handle(cur_heh) == cur_fh);

        // get local edge configuration
        HEH prev_heh = tri_mesh_.prev_halfedge_handle(cur_heh);
        HEH next_heh = tri_mesh_.next_halfedge_handle(cur_heh);

//        Point_2 uv0l(_uv_coords[2*prev_heh.idx()], _uv_coords[2*prev_heh.idx()+1]);
        Point_2 uv1l(_uv_coords[2*cur_heh .idx()], _uv_coords[2*cur_heh .idx()+1]);
        Point_2 uv2l(_uv_coords[2*next_heh.idx()], _uv_coords[2*next_heh.idx()+1]);

        // identify intersecting edge
        if(path.intersects(Segment_2(uv1l, uv2l)))
          cur_heh = next_heh;
        else
        {
          // other edge *must* intersect
          //assert(path.intersects(Segment_2(Point_2(_uv_coords[2*prev_heh.idx()], _uv_coords[2*prev_heh.idx()+1]), uv2l)));
          cur_heh = prev_heh;
        }
      }break;

      // ############# CASE OnVertex ###############
      case GridVertex::OnVertex:
      {
        VH vh = tri_mesh_.to_vertex_handle(_gv.heh);

        if(tri_mesh_.to_vertex_handle(heh0) == vh)
            cur_heh = heh2;
        else if(tri_mesh_.to_vertex_handle(heh1) == vh)
            cur_heh = heh0;
        else if(tri_mesh_.to_vertex_handle(heh2) == vh)
            cur_heh = heh1;
        else {
            std::cerr << "ERROR: triangle does not contain required vertex!!!" << std::endl;
            std::cerr << "vh idx: " << vh.idx() << ", fh idx: " << cur_fh.idx() << std::endl;
            return FindPathResult::Error();
        }
      }break;
    }
  }

  if(!cur_heh.is_valid())
  {
    std::cerr << "Warning: invalid heh after initialization!" << std::endl;
    return FindPathResult::Error();
  }

  // walk to next face
  if(!edge_valid_[tri_mesh_.edge_handle(cur_heh).idx()]) {
    // ran into degeneracy
    return FindPathResult::Signal(LocalEdgeInfo::LECI_Traced_Into_Degeneracy);
  }
  TF tf = transition(cur_heh);
  tf.transform_point(uv_from);
  tf.transform_point(uv_original_from);
  tf.transform_point(uv_to);
  accumulated_tf = tf * accumulated_tf;
  cur_heh = tri_mesh_.opposite_halfedge_handle(cur_heh);

  // #################### MAIN WALKING LOOP #######################
  // maximal number of steps as a safeguard
  unsigned int walk_iterations = 0;
  for(; walk_iterations<100000; ++walk_iterations)
  {
    // ran into a boundary?
    if(tri_mesh_.is_boundary(cur_heh)) {
      return FindPathResult::Signal(LocalEdgeInfo::LECI_Traced_Into_Boundary);
    }

    // get current face handle
    cur_fh = tri_mesh_.face_handle(cur_heh);

    // get halfedges of triangle
    heh0 = cur_heh;
    heh1 = tri_mesh_.next_halfedge_handle(heh0);
    heh2 = tri_mesh_.next_halfedge_handle(heh1);

    // get points
    uv0 = Point_2(_uv_coords[2*heh0.idx()], _uv_coords[2*heh0.idx()+1]);
    uv1 = Point_2(_uv_coords[2*heh1.idx()], _uv_coords[2*heh1.idx()+1]);
    uv2 = Point_2(_uv_coords[2*heh2.idx()], _uv_coords[2*heh2.idx()+1]);

    // get triangle
    tri = Triangle_2(uv0, uv1, uv2);
    const ORIENTATION tri_ori = tri.orientation();

    if (tri_ori == ORI_ZERO) {
        if (uv0 != uv1 && uv1 != uv2 && uv2 != uv0) {
            std::cerr
                << "\x1b[41mLogic error: Traced into degenerate triangle (a "
                   "cap). This shouldn't be possible.\x1b[0m"
                << std::endl;
            //assert(false);
            //return FindPathResult::Signal(LocalEdgeInfo::LECI_Traced_Into_Degeneracy);
        } else {
            std::cerr << "\x1b[41mEdges degenerated to a point should have "
                    << std::endl <<
                    "been removed during pre processing. This doesn't seem to "
                    << std::endl <<
                    "be the case here. Let's see how this ends.\x1b[0m"
                    << std::endl;
            return FindPathResult::Signal(LocalEdgeInfo::LECI_Traced_Into_Degeneracy);
        }
    }

    {
        const bool currentlyInverted = (tri_ori == ORI_NEGATIVE);
        if (currentlyInverted != inverted) {
            inverted = currentlyInverted;
            std::swap(uv_from, uv_to);
        }
    }

    // found endpoint?
    const BOUNDEDNESS bs = tri.boundedness(uv_to);
    if (bs == BND_ON_BOUNDED_SIDE  || bs == BND_ON_BOUNDARY)
    {
      return find_local_connection(uv_from, uv_original_from, uv_to, tri, heh0, heh1, heh2, bs, accumulated_tf, _uv_coords);
    }
    else // move forward
    {
      const Segment_2 path(uv_from, uv_to);
      const Segment_2   s1(    uv0,   uv1);
      const Segment_2   s2(    uv2,   uv1);

      const bool is1 = path.intersects(s1);
      const bool is2 = path.intersects(s2);

      HEH heh_upd(-1);

      if( is1 && !is2)
      {
        heh_upd = heh1;
      }
      else if( !is1 && is2)
      {
        heh_upd = heh2;
      }
      else if( is1 && is2)
      {
        const bool vis0 = path.has_on(uv0);
        const bool vis1 = path.has_on(uv1);
        const bool vis2 = path.has_on(uv2);

        if (!vis0 && !vis1 && vis2) {
            heh_upd = heh1;
        } else if (vis0 && vis2) {
            /*
             * Were on cur_heh. Check whether we have to leave through
             * heh1 or heh2
             */
            if (orient2d(path[0], path[1], uv1) == tri_ori)
                heh_upd = heh1;
            else
                heh_upd = heh2;
        } else {
            heh_upd = heh2;
        }
      }
      else if( !is1 && !is2)
      {
        std::cerr
            << "\x1b[1;41m"
            << "Warning: find_path didn't find the point where the path "
               "leaves a triangle in step " << walk_iterations << "." << std::endl
            << "*********** DEBUG OUTPUT START ***********\x1b[0m" << std::endl;
        std::cerr << "triangle-path intersection: " << int(path.intersects(tri)) << std::endl;
        std::cerr << "Segment 1: " << s1 << "," << std::endl
                << "Segment 2: " << s2 << "," << std::endl
                << "Path: " << path << std::endl
                << "Here's Tikz output for you so you can debug it more easily. You're welcome." << std::endl
                << s1.toTikz() << std::endl << s2.toTikz() << std::endl << path.toTikz() << std::endl;
        const bool vis0 = path.has_on(uv0);
        const bool vis1 = path.has_on(uv1);
        const bool vis2 = path.has_on(uv2);
        std::cerr << "Debug info:" << std::endl
                << "vis{0,1,2} = " << vis0 << ", " << vis1
                    << ", " << vis2 << std::endl
                << "is1, is2 = " << is1 << ", " << is2 << std::endl
                << "uv{0,1,2} = " << uv0 << ", " << uv1
                << ", " << uv2 << "" << std::endl
                << "path = " << path[0] << " -> " << path[1] << std::endl
                << "orient2d(uv0, uv2, path[0]) = " << static_cast<int>(
                        orient2d(uv0, uv2, path[0])) << std::endl
                << "orient2d(uv0, uv2, path[1]) = " << static_cast<int>(
                        orient2d(uv0, uv2, path[1])) << std::endl
                << "tri_ori = " << tri_ori << std::endl
                ;

        std::cerr
            << "\x1b[1;41m"
            << "*********** DEBUG OUTPUT END ***********\x1b[0m" << std::endl;
        // return error
        return FindPathResult::Error();
      }

      if(!heh_upd.is_valid())
      {
        std::cerr << "Warning: marching lead to invalid next heh!" << std::endl;
        return FindPathResult::Error();
      }


      if(!edge_valid_[tri_mesh_.edge_handle(heh_upd).idx()]) {
        // ran into degeneracy
          return FindPathResult::Signal(LocalEdgeInfo::LECI_Traced_Into_Degeneracy);
      }

      TF tf = transition(heh_upd);
      tf.transform_point(uv_from);
      tf.transform_point(uv_original_from);
      tf.transform_point(uv_to);
      accumulated_tf = tf * accumulated_tf;
      cur_heh = tri_mesh_.opposite_halfedge_handle(heh_upd);
    }
  }

  std::cerr << "\x1b[41mWarning: Maximum number of iterations exceeded in "
            "find_path. Diagnostics follow." << std::endl
            << "\x1b[41m--------------------------------------------------"
            "------------------------------\x1b[0m" << std::endl
            << "walk_iterations: " << walk_iterations << std::endl
            << "start gv (_gv): " << _gv << std::endl
            << "trace lei: " << lei << std::endl
            << "uv coords of _gv.heh and previous ("
            << uv_as_vec2d(_gv.heh, _uv_coords) << "), ("
            << uv_as_vec2d(tri_mesh_.prev_halfedge_handle(_gv.heh), _uv_coords)
            << ")" << std::endl;
  std::cerr << "\x1b[41m--------------------------------------------------"
            "------------------------------\x1b[0m" << std::endl;

  // return error
  return FindPathResult::Error();
}


//-----------------------------------------------------------------------------

template<class TMeshT>
typename MeshExtractorT<TMeshT>::FindPathResult
MeshExtractorT<TMeshT>::
find_local_connection(const Point_2& _uv_from, const Point_2& _uv_original_from, const Point_2& _uv_to, const Triangle_2& _tri,
                      const HEH _heh0, const HEH _heh1, const HEH _heh2, const BOUNDEDNESS& _bs,
                      TF &accumulated_tf, std::vector<double>& _uv_coords)
{
  if (_tri.is_degenerate())
      return FindPathResult::Signal(LocalEdgeInfo::LECI_Traced_Into_Degeneracy);

  assert( _bs == BND_ON_BOUNDED_SIDE || _bs == BND_ON_BOUNDARY);

  // strictly inside triangle ?
  if( _bs == BND_ON_BOUNDED_SIDE)
  {
    // get face handle
    const FH fh = tri_mesh_.face_handle(_heh0);
    const ORIENTATION face_ori = triangleUvOrientation(fh, _uv_coords);

    // get index of outgoing-direction of target vertex
    const Vector_2 dir = _uv_from-_uv_to;
    const int ori_idx  = face_ori == ORI_NEGATIVE ? ori_to_idx_inverse(dir) : ori_to_idx(dir);

    for(unsigned int i=0; i<face_gvertices_[fh.idx()].size(); ++i)
    {
      int gvidx = face_gvertices_[fh.idx()][i];

      assert((int)gvertices_[gvidx].local_edges.size() > ori_idx);

      if (gvertices_[gvidx].local_edges[ori_idx].uv_intended_to   == _uv_from && gvertices_[gvidx].local_edges[ori_idx].uv_from == _uv_to)
      {
          Point_2 from = _uv_original_from, to = _uv_to;
          reverseApply(from, to, accumulated_tf);
          return FindPathResult(gvidx, ori_idx, from, to, accumulated_tf);
      }
    }
  }
  else // on boundary
  {

    /* test vertices */
    if(_uv_to == _tri[0])
      return find_local_connection_at_vertex(_uv_from, _uv_original_from, _uv_to, _heh0, _tri, accumulated_tf);
    else if(_uv_to == _tri[1])
      return find_local_connection_at_vertex(_uv_from, _uv_original_from, _uv_to, _heh1, Triangle_2(_tri[1],_tri[2],_tri[0]), accumulated_tf);
    else if(_uv_to == _tri[2])
      return find_local_connection_at_vertex(_uv_from, _uv_original_from, _uv_to, _heh2, Triangle_2(_tri[2],_tri[0],_tri[1]), accumulated_tf);

    /* test edges */
    else if( Segment_2(_tri[2], _tri[0]).has_on(_uv_to))
      return find_local_connection_at_edge(_uv_from, _uv_original_from, _uv_to, _heh0, accumulated_tf);
    else if( Segment_2(_tri[0], _tri[1]).has_on(_uv_to))
      return find_local_connection_at_edge(_uv_from, _uv_original_from, _uv_to, _heh1, accumulated_tf);
    else  if( Segment_2(_tri[1], _tri[2]).has_on(_uv_to))
      return find_local_connection_at_edge(_uv_from, _uv_original_from, _uv_to, _heh2, accumulated_tf);
  }

  std::cerr << "Warning: find_local_connection did not succeed!" << std::endl;
  // return error
  return FindPathResult::Error();
}


//-----------------------------------------------------------------------------


template<class TMeshT>
typename MeshExtractorT<TMeshT>::FindPathResult
MeshExtractorT<TMeshT>::
find_local_connection_at_edge(const Point_2& _uv_from, const Point_2& _uv_original_from, const Point_2& _uv_to, const HEH _heh, TF &accumulated_tf)
{
  // get edge and face handle
  EH eh = tri_mesh_.edge_handle(_heh);
  FH fh = tri_mesh_.face_handle(_heh);

  // get data for opposite face
  HEH heh_opp = tri_mesh_.opposite_halfedge_handle(_heh);
  FH fh_opp(-1);
  if(!tri_mesh_.is_boundary(heh_opp))
    fh_opp = tri_mesh_.face_handle(heh_opp);

  Point_2 uv_from_opp(_uv_from);
  Point_2 uv_original_from_opp(_uv_original_from);
  Point_2 uv_to_opp  (_uv_to);
  const TF cross_edge_tf = transition(_heh);
  cross_edge_tf.transform_point(uv_from_opp);
  cross_edge_tf.transform_point(uv_to_opp);

  // check all vertices on this edge
  for(unsigned int i=0; i<edge_gvertices_[eh.idx()].size(); ++i) {
    const int vidx = edge_gvertices_[eh.idx()][i];
    // check all outgoing edges for both sides
    for (unsigned int j = 0; j < gvertices_[vidx].local_edges.size(); ++j) {
        if ((      gvertices_[vidx].local_edges[j].fh_from == fh
                && gvertices_[vidx].local_edges[j].uv_from == _uv_to
                && gvertices_[vidx].local_edges[j].uv_intended_to == _uv_from)

               || (gvertices_[vidx].local_edges[j].fh_from == fh_opp
                && gvertices_[vidx].local_edges[j].uv_from == uv_to_opp
                && gvertices_[vidx].local_edges[j].uv_intended_to == uv_from_opp)) {

            Point_2 from, to;
            if (tri_mesh_.face_handle(gvertices_[vidx].heh) == fh) {
                from = _uv_original_from;
                to = _uv_to;
            } else {
                assert(tri_mesh_.face_handle(gvertices_[vidx].heh) == fh_opp);
                from = uv_original_from_opp;
                to = uv_to_opp;
                accumulated_tf = cross_edge_tf * accumulated_tf;
            }
            reverseApply(from, to, accumulated_tf);
            return FindPathResult(vidx, j, from, to, accumulated_tf);
        }
    }
  }

  // return error
  return FindPathResult::Error();
}


//-----------------------------------------------------------------------------


template<class TMeshT>
typename MeshExtractorT<TMeshT>::FindPathResult
MeshExtractorT<TMeshT>::
find_local_connection_at_vertex(const Point_2& _uv_from, const Point_2& _uv_original_from, const Point_2& _uv_to, const HEH _heh, const Triangle_2& _tri, TF &accumulated_tf)
{
  // conventions
  // _heh points to vertex where _uv_to lies
  // _tri is in local ordering _tri[0] = uv(_heh->to)

  // get vertex handle
  VH vh = tri_mesh_.to_vertex_handle(_heh);

  // create candidate set
  std::vector<FH>       fh;      fh.reserve(2);
  std::vector<TF>       tfs;     tfs.reserve(2);
  std::vector<Point_2>  uv_from; uv_from.reserve(2);
  std::vector<Point_2>  uv_original_from; uv_original_from.reserve(2);
  std::vector<Point_2>  uv_to;   uv_to.reserve(2);

  // check this face
  fh     .push_back(tri_mesh_.face_handle(_heh));
  uv_from.push_back(_uv_from);
  uv_original_from.push_back(_uv_original_from);
  uv_to  .push_back(_uv_to);
  tfs.push_back(TF::IDENTITY);

  // have to check CCW-neighboring face?
  if(is_collinear(_uv_from, _uv_to, _tri[2]))
  {
    HEH opp_heh = tri_mesh_.opposite_halfedge_handle(_heh);
    if(!tri_mesh_.is_boundary(opp_heh))
    {
      fh     .push_back(tri_mesh_.face_handle(opp_heh));
      uv_from.push_back(_uv_from);
      uv_original_from.push_back(_uv_original_from);
      uv_to  .push_back(_uv_to);
      // transform points
      TF tf = transition(_heh);
      tf.transform_point(uv_from.back());
      tf.transform_point(uv_original_from.back());
      tf.transform_point(uv_to.back());
      tfs.push_back(tf);
    }
  }

  // have to check CW-neighboring face?
  if(is_collinear(_uv_from, _uv_to, _tri[1]))
  {
    HEH nheh     = tri_mesh_.next_halfedge_handle(_heh);
    HEH opp_nheh = tri_mesh_.opposite_halfedge_handle(nheh);
    if(!tri_mesh_.is_boundary(opp_nheh))
    {
      fh     .push_back(tri_mesh_.face_handle(opp_nheh));
      uv_from.push_back(_uv_from);
      uv_original_from.push_back(_uv_original_from);
      uv_to  .push_back(_uv_to);
      // transform points
      TF tf = transition(nheh);
      tf.transform_point(uv_from.back());
      tf.transform_point(uv_original_from.back());
      tf.transform_point(uv_to.back());
      tfs.push_back(tf);
    }
  }

  // check all candidates
  for(unsigned int i=0; i<vertex_gvertices_[vh.idx()].size(); ++i)
  {
    // get global vertex idx
    int vidx = vertex_gvertices_[vh.idx()][i];
    // test all candidates outgoing edges
    for(unsigned int j=0; j<gvertices_[vidx].local_edges.size(); ++j)
      // test all generated candidates
      for(unsigned int k=0; k<fh.size(); ++k)
      {
        if(fh     [k] == gvertices_[vidx].local_edges[j].fh_from &&
           uv_from[k] == gvertices_[vidx].local_edges[j].uv_intended_to   &&
           uv_to  [k] == gvertices_[vidx].local_edges[j].uv_from    )
        {
            const TF intra_gv_trans = intra_gv_transition(fh[k], tri_mesh_.face_handle(gvertices_[vidx].heh), gvertices_[vidx], true);
            accumulated_tf = intra_gv_trans * tfs[k] * accumulated_tf;
          Point_2 from = uv_original_from[k];
          Point_2 to = uv_to[k];
          intra_gv_trans.transform_point(from);
          intra_gv_trans.transform_point(to);
          reverseApply(from, to, accumulated_tf);
          return FindPathResult(vidx, j, from, to, accumulated_tf);
        }
      }
  }

  // return error
  return FindPathResult::Error();
}


//-----------------------------------------------------------------------------

template<class TMeshT>
template<class PolyMeshT>
void
MeshExtractorT<TMeshT>::
generate_faces_and_store_quadmesh(PolyMeshT& _quad_mesh,
        typename PropMgr<PolyMeshT>::LocalUvsPropertyManager &heLocalUvProp)
{
  // clear old data
  _quad_mesh.clear();
  _quad_mesh.request_vertex_status();
  tri_mesh_.request_vertex_status();

  // add vertices and set boundary tag
  for(unsigned int i=0; i<gvertices_.size(); ++i)
  {
    typename PolyMeshT::VertexHandle vh = _quad_mesh.add_vertex((typename PolyMeshT::Point)gvertices_[i].position_3d);
    _quad_mesh.status(vh).set_tagged(gvertices_[i].is_boundary);
  }

  for(unsigned int i=0; i<gvertices_.size(); ++i) {
    for(unsigned int j=0; j<gvertices_[i].n_edges(); ++j) {
      // not already constructed? -> start new face
      if(!gvertices_[i].local_edges[j].face_constructed)
      {
        // build vector of vertex handles
        std::vector<typename PolyMeshT::VertexHandle> face_vhs;
        std::vector<LocalEdgeInfo*> outgoing_he_info;

        int current_gvertex_idx = i;
        int current_orientation_idx = j;

        // maximally allow faces with 100 vertices (safe-guard against infinite loops)
        for(unsigned int k=0; k<100; ++k)
        {
          // valid connection?
          if( current_gvertex_idx >= 0)
          {
            // returned to start?
            if(current_gvertex_idx == (int)i && face_vhs.size())
            {
              // valid face? -> add
              if(face_vhs.size() > 2)
              {
                  // add face
#ifdef DISCARD_FACES_WITH_DOUBLE_VERTICES
                  typename PolyMeshT::FaceHandle fh = _quad_mesh.add_face(face_vhs);
#else
                  typename PolyMeshT::FaceHandle fh = add_face<PolyMeshT>(_quad_mesh, outgoing_he_info);
#endif
                  if (!fh.is_valid()) {
                      std::cerr << "\x1b[41mSkipping face. (OpenMesh doesn't support non-manifold meshes. -> Might lead to a hole.)\x1b[0m" << std::endl
                              ;
                      break;
                  }
                  /*
                   * Advance iterator to first vertex in list.
                   */
                  typename PolyMeshT::FaceHalfedgeIter fhi_it, fhi_it_begin, fhi_it_end;
                  for (fhi_it_begin = _quad_mesh.fh_begin(fh), fhi_it = fhi_it_begin, fhi_it_end = _quad_mesh.fh_end(fh); fhi_it != fhi_it_end; ++fhi_it) {
                      const typename PolyMeshT::VertexHandle vh = _quad_mesh.from_vertex_handle(*fhi_it);
                      if (vh == face_vhs.front()) break;
                  }
                  if (fhi_it == fhi_it_end) throw std::logic_error("Expected vertex not found in face");

                  /*
                   * Transfer halfedge param info onto newly created face's halfedges.
                   */
                  TF accumulated_face_tf = TF::IDENTITY;
                  const LocalEdgeInfo *last_lei = 0, *last_opp_lei = 0;
                  for (typename std::vector<LocalEdgeInfo*>::const_iterator lei_it = outgoing_he_info.begin(), lei_end = outgoing_he_info.end();
                          lei_it != lei_end; last_lei = *(lei_it++), ++fhi_it) {

                      /*
                       * We came via local edge last_lei.
                       * Counterpart is last_opp_lei := gvertices_[last_lei.connected_to_idx].local_edge(last_lei.orientation_idx).
                       * Next local edge is *lei_it.
                       *
                       * We need the accumulated transfer function from opp_lei->fh_from to lei_it->fh_from.
                       *
                       * Note that those two faces are on the same edge fan, sharing the vertex
                       * tri_mesh_.to_vertex_handle(gvertices_[last_lei.connected_to_idx].heh).
                       */

                      TF intra_vertex_tf = TF::IDENTITY;

                      if (last_lei) {
                          last_opp_lei = &gvertices_[last_lei->connected_to_idx].local_edge(last_lei->orientation_idx);
                          assert(*last_lei == gvertices_[last_opp_lei->connected_to_idx].local_edge(last_opp_lei->orientation_idx));
                          intra_vertex_tf =
                                  intra_gv_transition(last_opp_lei->fh_from, (*lei_it)->fh_from, gvertices_[last_lei->connected_to_idx], last_opp_lei != (*lei_it)) *
                                  intra_gv_transition(last_opp_lei->fh_from, tri_mesh_.face_handle(gvertices_[last_lei->connected_to_idx].heh), gvertices_[last_lei->connected_to_idx], true).inverse();
                      }
                      accumulated_face_tf = intra_vertex_tf * accumulated_face_tf;

                      /*
                       * lei_it.uv_to is in the coordinate system of
                       * lei_it.uv_from which is why we multiply
                       * lei_it.accumulated_tf onto accumulated_face_tf
                       * after we transformed uv_to.
                       */

                      Point_2 uv((*lei_it)->uv_to);
                      //std::cerr << "last_uv = (" << uv << ") * (" << accumulated_face_tf << ")^-1 = (" << uv << ") * (" << accumulated_face_tf.inverse() << ") = ";
                      accumulated_face_tf.inverse().transform_point(uv);
                      //std::cerr << "(" << uv << ")" << std::endl;

                      heLocalUvProp[*fhi_it] = Vec2i(uv[0], uv[1]);

                      accumulated_face_tf = (*lei_it)->accumulated_tf * accumulated_face_tf;
                  }
              }
              break;
            }
            else
            {
              // already constructed?
              if(gvertices_[current_gvertex_idx].local_edge(current_orientation_idx).face_constructed)
              {
                // removed output since this can happen on incomplete boundaries or degeneracies
                //                std::cerr << "Warning: construction entered a halfedge belonging to an already constructed face..." << std::endl;
                break;
              }

              // add vertex to face
              typename PolyMeshT::VertexHandle new_vh = _quad_mesh.vertex_handle(current_gvertex_idx);

              // don't add vertices twice
////              assert(std::find(face_vhs.begin(), face_vhs.end(), new_vh) == face_vhs.end());
#ifdef DISCARD_FACES_WITH_DOUBLE_VERTICES
              if(std::find(face_vhs.begin(), face_vhs.end(), new_vh) != face_vhs.end()) {
                //throw std::logic_error("Warning: face should not have a double vertex.");
                std::cerr << "Warning: face should not have a double vertex." << std::endl;
                break;
              }
#endif
              face_vhs.push_back(new_vh);

              // mark as traversed
              gvertices_[current_gvertex_idx].local_edge(current_orientation_idx).face_constructed = true;
              // get opposite
              {
                  LocalEdgeInfo &lei = gvertices_[current_gvertex_idx].local_edge(current_orientation_idx);
                  current_gvertex_idx = lei.connected_to_idx;
                  current_orientation_idx = lei.orientation_idx;

                  outgoing_he_info.push_back(&lei);
              }

              current_orientation_idx -= 1;
            }
          }
          else
          {
            break; // without constructing a face
          }
        }
      }
    }
  }

  //count number of unwanted holes and print information
  int n_undesired_holes(0);
  int n_desired_holes(0);
  int n_isolated_vertices_removed(0);
  std::set<typename PolyMeshT::VertexHandle> visited;
  typename PolyMeshT::VertexIter v_it  = _quad_mesh.vertices_sbegin();
  typename PolyMeshT::VertexIter v_end = _quad_mesh.vertices_end();
  for(; v_it != v_end; ++v_it)
  {
    if( !_quad_mesh.status(*v_it).deleted()&&
         _quad_mesh.is_boundary(*v_it)     &&
        !_quad_mesh.status(*v_it).tagged() &&
        !visited.count(*v_it)                )
    {
      typename PolyMeshT::VOHIter voh_it = _quad_mesh.voh_iter(*v_it);
      if(voh_it.is_valid())
      {
        typename PolyMeshT::HalfedgeHandle heh_start = *_quad_mesh.voh_iter(*v_it);
        typename PolyMeshT::HalfedgeHandle heh       = heh_start;

        std::vector<VH> cur_boundary; cur_boundary.reserve(1024);
        bool found_tagged = false;

        // boundary should always be closed!!! 100000 is a safeguard against infinite loops
        for(unsigned int i=0; i<100000; ++i)
        {
          // get current vertex_handle
          VH cur_vh = _quad_mesh.to_vertex_handle(heh);
          // mark as visited
          visited.insert(cur_vh);
          // add to current boundary
          cur_boundary.push_back(cur_vh);
          // tagged ?
          if(_quad_mesh.status(cur_vh).tagged())
            found_tagged = true;

          // go to next halfedge
          heh = _quad_mesh.next_halfedge_handle(heh);

          // finished loop?
          if(heh == heh_start)
            break;
        }

        // found wanted boundary? -> tag all on this boundary
        if( found_tagged )
        {
          ++n_desired_holes;
          for(unsigned int i=0; i<cur_boundary.size(); ++i)
            _quad_mesh.status(cur_boundary[i]).set_tagged(true);
        }
        else // found undesired hole
          ++n_undesired_holes;
      }
    }

    // isolated vertex ? -> remove if desired
     if( ! _quad_mesh.status(*v_it).deleted() && _quad_mesh.valence(*v_it) == 0)
     {
       // remove isolated vertex
       _quad_mesh.delete_vertex(*v_it, true);
       ++n_isolated_vertices_removed;
     }
  }
  // garbage collection if vertex was removed...
  if(n_isolated_vertices_removed)
    _quad_mesh.garbage_collection();


  // tag visualization for debugging -> deactivated per default
  if(0)
  {
    v_it  = _quad_mesh.vertices_sbegin();
    v_end = _quad_mesh.vertices_end();
    for(; v_it != v_end; ++v_it)
      if(_quad_mesh.status(*v_it).tagged())
        _quad_mesh.status(*v_it).set_feature(true);
  }

  _quad_mesh.update_normals();
}


//-----------------------------------------------------------------------------


template<class TMeshT>
typename MeshExtractorT<TMeshT>::TF
MeshExtractorT<TMeshT>::
transition(const VH& _vh) const
{
  if(tri_mesh_.is_boundary(_vh))
    return TF::identity();
  else
  {
    // start with identity
    TF tf(0,0,0);

    CVIHIter vih_it = tri_mesh_.cvih_iter(_vh);

    // store first heh-transition which should be the last for chart belonging to *vih_it
    TF tf_first = transition(tri_mesh_.opposite_halfedge_handle(*vih_it));
    ++vih_it;

    for(; vih_it.is_valid(); ++vih_it)
    {
      // compose with previous transformations
      tf = transition(tri_mesh_.opposite_halfedge_handle(*vih_it))*tf;
    }
    return tf_first*tf;
  }
}


//-----------------------------------------------------------------------------


template<class TMeshT>
Matrix_3
MeshExtractorT<TMeshT>::
get_mapping(const Triangle_2& _tri, const Point& _a, const Point& _b, const Point& _c) const
{
  assert(!_tri.is_degenerate());

  // 2d matrix
  Matrix_3 p;
  p << _tri[0][0], _tri[1][0], _tri[2][0],
       _tri[0][1], _tri[1][1], _tri[2][1],
                1,          1,          1;
  // 3d matrix
  Matrix_3 P;
  P << _a[0], _b[0], _c[0],
       _a[1], _b[1], _c[1],
       _a[2], _b[2], _c[2];

  return (P*p.inverse());
}


//-----------------------------------------------------------------------------


template<class TMeshT>
Matrix_3
MeshExtractorT<TMeshT>::
get_mapping(const Segment_2& _seg, const Point& _a, const Point& _b) const
{
  assert(!_seg.is_degenerate());

  // first construct (least-squares) expression for alpha
  // alpha = c^t(x)+d
  Vector_2 c(_seg[1][0]-_seg[0][0],_seg[1][1]-_seg[0][1]);
  c /= c.dot(c);
  double d = (-c).dot(Vector_2(_seg[0][0],_seg[0][1]));

  // then set up mapping
  Vec3d a(_a[0],_a[1],_a[2]);
  Vec3d b(_b[0],_b[1],_b[2]);

  Matrix_3 M;
  M.col(2) = a + d*(b-a);
  M.block<3, 2>(0,0) = (b-a)*c.transpose();
  return M;
}

template<class TMeshT>
template<class PolyMeshT>
void
MeshExtractorT<TMeshT>::
print_quad_mesh_metrics(const PolyMeshT& _quad_mesh) {
    std::map<unsigned int, int> valence_histogram;
    for (typename PolyMeshT::FaceIter f_it = _quad_mesh.faces_sbegin(), f_end = _quad_mesh.faces_end(); f_it != f_end; ++f_it) {
        assert(f_it->is_valid());
        const unsigned int valence = _quad_mesh.valence(*f_it);
        valence_histogram[valence] += 1;
    }

    std::cout << "Face valence histogram:" << std::endl;
    for (std::map<unsigned int, int>::const_iterator it = valence_histogram.begin(); it != valence_histogram.end(); ++it) {
        std::cout << "  Valence " << it->first << ": " << it->second << std::endl;
    }

    if (valence_histogram.size() > 1 || valence_histogram.find(4) == valence_histogram.end()) {
        std::cout << "  \x1b[41mThis is not a quad mesh!\x1b[0m" << std::endl;
    }
}

template<class TMeshT>
typename MeshExtractorT<TMeshT>::TF MeshExtractorT<TMeshT>::intra_gv_transition(FH from_fh, FH to_fh, const GridVertex &gv, bool returnIdentityIfSameFh) {
    if (returnIdentityIfSameFh && from_fh == to_fh) return TF::IDENTITY;

    if (gv.type == GridVertex::OnFace)
        return TF::IDENTITY;

    if (gv.type == GridVertex::OnEdge) {
        if (tri_mesh_.face_handle(gv.heh) == from_fh) {
            return (from_fh == to_fh ? transition(tri_mesh_.opposite_halfedge_handle(gv.heh)) : TF::IDENTITY) * transition(gv.heh);
        } else if (tri_mesh_.face_handle(tri_mesh_.opposite_halfedge_handle(gv.heh)) == from_fh) {
            return (from_fh == to_fh ? transition(gv.heh) : TF::IDENTITY) * transition(tri_mesh_.opposite_halfedge_handle(gv.heh));
        } else {
            throw std::logic_error("Grid Vertex' halfedge is not the one between requested faces.");
        }
    } else if (gv.type == GridVertex::OnVertex) {
        TF result(TF::IDENTITY);
        typename TMeshT::VIHIter vih_it, vih_end;
        const VH pivot = tri_mesh_.to_vertex_handle(gv.heh);
        for (vih_it = tri_mesh_.vih_begin(pivot), vih_end = tri_mesh_.vih_end(pivot);
                tri_mesh_.face_handle(*vih_it) != from_fh && vih_it != vih_end; ++vih_it);

        assert(tri_mesh_.face_handle(*vih_it) == from_fh);

        do {
            result = transition(tri_mesh_.next_halfedge_handle(*vih_it)) * result;
            ++vih_it;
        } while (tri_mesh_.face_handle(*vih_it) != to_fh);

        assert(tri_mesh_.face_handle(*vih_it) == to_fh);

        return result;
    } else {
        throw std::logic_error("Requested transition for neither OnEdge nor OnVertex Grid Vertex.");
    }
}

/**
 * Starting with orientation_idx + direction, increments the orientation by
 * direction until a local edge is found that is connected (i.e. does not
 * run into a boundary, etc.).
 * When it cycles back to orientation_idx, it returns 0.
 */
template<class TMeshT>
typename MeshExtractorT<TMeshT>::LocalEdgeInfo *MeshExtractorT<TMeshT>::getNextConnectedLeiWithHE(int connected_to_idx, int orientation_idx, int direction) {

    LocalEdgeInfo *lei = 0, *last = 0;
    for (last = &gvertices_[connected_to_idx].local_edge(orientation_idx);
            lei != last && (!lei || !lei->isConnected() || lei->halfedgeIndex == -1);
            lei = &gvertices_[connected_to_idx].local_edge(orientation_idx += direction));

    return lei;
}

template<class TMeshT>
template<class PolyMeshT>
typename PolyMeshT::FaceHandle MeshExtractorT<TMeshT>::add_face(PolyMeshT &qmesh, std::vector<typename MeshExtractorT<TMeshT>::LocalEdgeInfo*> &leis) {
    typedef typename std::vector<LocalEdgeInfo*> LEI_VEC;

    assert(!leis.empty());

    /*
     * Check if operation will yield manifold result.
     */
    for (typename LEI_VEC::iterator lei_it = leis.begin(), lei_end = leis.end(); lei_it != lei_end; ++lei_it) {
        /*
         * If one of the halfedges we want to connect to a face already has a face then
         * that means we would get a non-manifold configuration.
         */
        if ((*lei_it)->halfedgeIndex != -1 && qmesh.face_handle(qmesh.halfedge_handle((*lei_it)->halfedgeIndex)).is_valid())
            return typename PolyMeshT::FaceHandle();

        if ((*lei_it)->halfedgeIndex == -1) {
            LocalEdgeInfo * const opposite_lei = &gvertices_[(*lei_it)->connected_to_idx].local_edge((*lei_it)->orientation_idx);
            LocalEdgeInfo * const opposite_next_lei = getNextConnectedLeiWithHE(opposite_lei->connected_to_idx, opposite_lei->orientation_idx, -1);
            LocalEdgeInfo * const opposite_opp_prev_lei = getNextConnectedLeiWithHE((*lei_it)->connected_to_idx, (*lei_it)->orientation_idx, 1);
            LocalEdgeInfo * const opposite_prev_lei = opposite_opp_prev_lei == 0 ? 0 :
                    &gvertices_[opposite_opp_prev_lei->connected_to_idx].local_edge(opposite_opp_prev_lei->orientation_idx);
            assert(opposite_prev_lei == 0 || opposite_prev_lei->connected_to_idx == (*lei_it)->connected_to_idx);
            if (opposite_next_lei && opposite_next_lei->halfedgeIndex != -1 && qmesh.face_handle(qmesh.halfedge_handle(opposite_next_lei->halfedgeIndex)).is_valid())
                return typename PolyMeshT::FaceHandle();
            if (opposite_prev_lei && opposite_prev_lei->halfedgeIndex != -1 && qmesh.face_handle(qmesh.halfedge_handle(opposite_prev_lei->halfedgeIndex)).is_valid())
                return typename PolyMeshT::FaceHandle();
        }
    }

    typename PolyMeshT::FaceHandle newFh = qmesh.new_face();

    /*
     * Add halfedges
     */
    for (typename LEI_VEC::iterator lei_it = leis.begin(), lei_end = leis.end(); lei_it != lei_end; ++lei_it) {
        typename PolyMeshT::HalfedgeHandle heh0;
        if ((*lei_it)->halfedgeIndex == -1) {
            LocalEdgeInfo * const opposite_lei = &gvertices_[(*lei_it)->connected_to_idx].local_edge((*lei_it)->orientation_idx);
            assert(opposite_lei->halfedgeIndex == -1);

            typename PolyMeshT::VertexHandle
                from_vh = qmesh.vertex_handle(opposite_lei->connected_to_idx),
                to_vh = qmesh.vertex_handle((*lei_it)->connected_to_idx);
            heh0 = qmesh.new_edge(from_vh, to_vh);
            const typename PolyMeshT::HalfedgeHandle heh1 = qmesh.opposite_halfedge_handle(heh0);

            if (!qmesh.halfedge_handle(from_vh).is_valid()) {
                qmesh.set_halfedge_handle(from_vh, heh0);
            }

            if (!qmesh.halfedge_handle(to_vh).is_valid()) {
                qmesh.set_halfedge_handle(to_vh, heh1);
            }

            (*lei_it)->halfedgeIndex = heh0.idx();
            opposite_lei->halfedgeIndex = heh1.idx();

            /*
             * Connect opposite halfedge to next and previous one if possible.
             */
            LocalEdgeInfo * const opposite_next_lei = getNextConnectedLeiWithHE(opposite_lei->connected_to_idx, opposite_lei->orientation_idx, -1);
            LocalEdgeInfo * const opposite_opp_prev_lei = getNextConnectedLeiWithHE((*lei_it)->connected_to_idx, (*lei_it)->orientation_idx, 1);
            LocalEdgeInfo * const opposite_prev_lei = opposite_opp_prev_lei == 0 ? 0 :
                    &gvertices_[opposite_opp_prev_lei->connected_to_idx].local_edge(opposite_opp_prev_lei->orientation_idx);
            assert(opposite_prev_lei == 0 || opposite_prev_lei->halfedgeIndex >= 0);
            assert(opposite_prev_lei == 0 || opposite_prev_lei->connected_to_idx == (*lei_it)->connected_to_idx);
            assert(!qmesh.next_halfedge_handle(heh1).is_valid());
            assert(!qmesh.prev_halfedge_handle(heh1).is_valid());
            //if (opposite_next_lei->halfedgeIndex != -1) {
            if (opposite_next_lei) {
                assert(!qmesh.face_handle(qmesh.halfedge_handle(opposite_next_lei->halfedgeIndex)).is_valid());
                qmesh.set_next_halfedge_handle(heh1, qmesh.halfedge_handle(opposite_next_lei->halfedgeIndex));
            }
            //if (opposite_prev_lei->halfedgeIndex != -1) {
            if (opposite_prev_lei) {
                assert(!qmesh.face_handle(qmesh.halfedge_handle(opposite_prev_lei->halfedgeIndex)).is_valid());
                qmesh.set_next_halfedge_handle(qmesh.halfedge_handle(opposite_prev_lei->halfedgeIndex), heh1);
            }

        } else {
            heh0 = qmesh.halfedge_handle((*lei_it)->halfedgeIndex);
        }

        if (lei_it == leis.begin())
            qmesh.set_halfedge_handle(newFh, heh0);
        qmesh.set_face_handle(heh0, newFh);
    }

    qmesh.set_next_halfedge_handle(qmesh.halfedge_handle(leis.back()->halfedgeIndex), qmesh.halfedge_handle(leis.front()->halfedgeIndex));
    assert(!qmesh.face_handle(qmesh.halfedge_handle(leis.front()->halfedgeIndex)).is_valid() || qmesh.face_handle(qmesh.halfedge_handle(leis.front()->halfedgeIndex)) == newFh);
    qmesh.set_face_handle(qmesh.halfedge_handle(leis.front()->halfedgeIndex), newFh);
    if (leis.size() > 1) {
        for (typename LEI_VEC::iterator lei_it = leis.begin(), prev_lei_it = lei_it++, lei_end = leis.end(); lei_it != lei_end; ++lei_it, ++prev_lei_it) {
            qmesh.set_next_halfedge_handle(qmesh.halfedge_handle((*prev_lei_it)->halfedgeIndex), qmesh.halfedge_handle((*lei_it)->halfedgeIndex));

            assert(!qmesh.face_handle(qmesh.halfedge_handle((*lei_it)->halfedgeIndex)).is_valid() || qmesh.face_handle(qmesh.halfedge_handle((*lei_it)->halfedgeIndex)) == newFh);
            qmesh.set_face_handle(qmesh.halfedge_handle((*lei_it)->halfedgeIndex), newFh);
        }
    }

    for (typename LEI_VEC::iterator lei_it = leis.begin(), lei_end = leis.end(); lei_it != lei_end; ++lei_it) {
        qmesh.adjust_outgoing_halfedge(qmesh.vertex_handle((*lei_it)->connected_to_idx));
    }

    return newFh;
}

} // namespace QEx
