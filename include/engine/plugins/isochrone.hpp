#ifndef ISOCHRONE_HPP
#define ISOCHRONE_HPP

#include "engine/api/isochrone_parameters.hpp"
#include "engine/plugins/isoline.hpp"
#include "osrm/json_container.hpp"
#include "util/binary_heap.hpp"

#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/function.hpp>
#include <tbb/parallel_sort.h>

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

namespace osrm
{
namespace engine
{
namespace plugins
{

using QueryHeap = osrm::util::
    BinaryHeap<NodeID, NodeID, int, HeapData, osrm::util::UnorderedMapStorage<NodeID, int>>;
typedef std::vector<IsochroneNode> IsochroneVector;

class IsochronePlugin final : public IsolinePlugin
{
  private:
    void dijkstraByDuration(const std::shared_ptr<datafacade::BaseDataFacade> facade,
                            IsochroneVector &set,
                            NodeID &source,
                            int duration);
    void update(IsochroneVector &s, IsochroneNode node);

  public:
    explicit IsochronePlugin();

    Status HandleRequest(const std::shared_ptr<datafacade::BaseDataFacade> facade,
                         const api::IsochroneParameters &params,
                         util::json::Object &json_result);
};
}
}
}

#endif // ISOCHRONE_HPP
