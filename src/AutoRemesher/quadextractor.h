/*
 *  Copyright (c) 2020 Jeremy HU <jeremy-at-dust3d dot org>. All rights reserved. 
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */
#ifndef AUTO_REMESHER_QUAD_EXTRACTOR_H
#define AUTO_REMESHER_QUAD_EXTRACTOR_H
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <AutoRemesher/Vector3>
#include <AutoRemesher/Vector2>

namespace AutoRemesher
{
    
class QuadExtractor
{
public:
    QuadExtractor(const std::vector<Vector3> *vertices,
            const std::vector<std::vector<size_t>> *triangles,
            const std::vector<std::vector<Vector2>> *triangleUvs) :
        m_vertices(vertices),
        m_triangles(triangles),
        m_triangleUvs(triangleUvs)
    {
    }
    
    const std::vector<Vector3> &remeshedVertices()
    {
        return m_remeshedVertices;
    }
    
    const std::vector<std::vector<size_t>> &remeshedQuads()
    {
        return m_remeshedQuads;
    }
    
    bool extract();
    
private:
    const std::vector<Vector3> *m_vertices = nullptr;
    const std::vector<std::vector<size_t>> *m_triangles = nullptr;
    const std::vector<std::vector<Vector2>> *m_triangleUvs = nullptr;
    std::vector<Vector3> m_remeshedVertices;
    std::vector<std::vector<size_t>> m_remeshedQuads;
    std::set<std::pair<size_t, size_t>> m_goodQuadHalfEdges;
    std::unordered_set<size_t> m_tVertices;
    
    void extractConnections(std::vector<Vector3> *crossPoints, 
        std::vector<size_t> *sourceTriangles,
        std::set<std::pair<size_t, size_t>> *connections);
    void extractEdges(const std::set<std::pair<size_t, size_t>> &connections,
        std::unordered_map<size_t, std::unordered_set<size_t>> *edgeConnectMap);
    bool collapseShortEdges(std::vector<Vector3> *crossPoints,
        std::unordered_map<size_t, std::unordered_set<size_t>> *edgeConnectMap);
    void collapseEdge(std::vector<Vector3> *crossPoints,
        std::unordered_map<size_t, std::unordered_set<size_t>> *edgeConnectMap,
        const std::pair<size_t, size_t> &edge);
    void extractMesh(std::vector<Vector3> &points,
        const std::vector<size_t> &pointSourceTriangles,
        std::unordered_map<size_t, std::unordered_set<size_t>> &edgeConnectMap,
        std::vector<std::vector<size_t>> *quads);
    void simplifyGraph(std::unordered_map<size_t, std::unordered_set<size_t>> &graph);
    void fixFlippedFaces();
    bool removeIsolatedFaces();
    bool removeNonManifoldFaces();
    void makeTedge(const std::vector<size_t> &triangle);
    void fixHoles();
    void fixHoleWithQuads(std::vector<size_t> &hole, bool checkScore=true);
    void recordGoodQuads();
    bool testPointInTriangle(const std::vector<Vector3> &points, 
        const std::vector<size_t> &triangle,
        const std::vector<size_t> &testPoints);
    void connectTvertices();
    void buildVertexNeighborMap(std::unordered_map<size_t, std::vector<size_t>> *vertexNeighborMap);
    void connectTwoTvertices(size_t startVertex, const std::vector<size_t> &path);
};
    
}

#endif
