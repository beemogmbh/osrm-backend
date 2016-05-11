//
// Created by robin on 4/6/16.
//

#ifndef ISOCHRONE_HPP
#define ISOCHRONE_HPP

#include "engine/plugins/plugin_base.hpp"
#include "engine/api/isochrone_parameters.hpp"
#include "util/static_graph.hpp"
#include "util/coordinate.hpp"
#include "osrm/json_container.hpp"
#include <util/binary_heap.hpp>

#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <tbb/parallel_sort.h>

#include <cstdlib>
#include <unordered_set>
#include <map>
#include <utility>
#include <vector>
#include <set>
#include <algorithm>

namespace osrm
{
namespace engine
{
namespace plugins
{
struct HeapData
{
    NodeID parent;
    HeapData(NodeID p) : parent(p) {}
};
struct SimpleEdgeData
{
    SimpleEdgeData() : weight(INVALID_EDGE_WEIGHT), real(false) {}
    SimpleEdgeData(unsigned weight_, bool real_) : weight(weight_), real(real_) {}
    unsigned weight;
    bool real;
};

struct IsochroneNode
{
    IsochroneNode(){};
    IsochroneNode(osrm::extractor::QueryNode node, osrm::extractor::QueryNode predecessor, int distance)
        : node(node), predecessor(predecessor), distance(distance)
    {
    }

    osrm::extractor::QueryNode node;
    osrm::extractor::QueryNode predecessor;
    int distance;
};
struct IsochroneNodeIdCompare
{
    bool operator()(const IsochroneNode &a, const IsochroneNode &b) const
    {
        return a.node.node_id < b.node.node_id;
    }
};
struct IsochroneNodeDistanceCompare
{
    bool operator()(const IsochroneNode &a, const IsochroneNode &b) const
    {
        return a.distance < b.distance;
    }
};

using SimpleGraph = util::StaticGraph<SimpleEdgeData>;
using SimpleEdge = SimpleGraph::InputEdge;
using QueryHeap = osrm::util::
    BinaryHeap<NodeID, NodeID, int, HeapData, osrm::util::UnorderedMapStorage<NodeID, int>>;
typedef std::set<IsochroneNode, IsochroneNodeIdCompare> IsochroneSet;
typedef std::set<IsochroneNode, IsochroneNodeDistanceCompare> IsochroneDistanceSet;

class IsochronePlugin final : public BasePlugin
{
  private:
    boost::filesystem::path base;
    std::shared_ptr<engine::plugins::SimpleGraph> graph;
    std::vector<osrm::extractor::QueryNode> coordinate_list;
    IsochroneSet isochroneSet;
    std::vector<engine::plugins::SimpleEdge> graph_edge_list;
    std::size_t number_of_nodes;

    std::size_t loadGraph(const std::string &path,
                          std::vector<extractor::QueryNode> &coordinate_list,
                          std::vector<SimpleEdge> &graph_edge_list);

    void update(IsochroneSet &s, IsochroneNode node);
    void findBorder(IsochroneSet &isochroneSet, util::json::Object &response);

  public:
    explicit IsochronePlugin(datafacade::BaseDataFacade &facade, const std::string base);

    Status HandleRequest(const api::IsochroneParameters &params, util::json::Object &json_result);
};
}
}
}

#endif // ISOCHRONE_HPP