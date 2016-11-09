#ifndef ENGINE_API_ISODISTANCE_PARAMETERS_HPP
#define ENGINE_API_ISODISTANCE_PARAMETERS_HPP

#include "engine/api/base_parameters.hpp"

namespace osrm
{
namespace engine
{
namespace api
{

struct IsodistanceParameters : public BaseParameters
{
    double distance = 0;
    bool convexhull = false;
    bool concavehull = false;
    double threshold = 1; // max precision

    bool IsValid() const { return BaseParameters::IsValid(); }
};
}
}
}
#endif // ENGINE_API_ISODISTANCE_PARAMETERS_HPP
