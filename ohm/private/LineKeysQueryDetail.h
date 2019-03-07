
// Copyright (c) 2017
// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
// ABN 41 687 119 230
//
// Author: Kazys Stepanas
#ifndef OHM_LINEKEYSQUERYDETAIL_H_
#define OHM_LINEKEYSQUERYDETAIL_H_

#include "OhmConfig.h"

#include "QueryDetail.h"

namespace ohm
{
  struct ohm_API LineKeysQueryDetail : QueryDetail
  {
    std::vector<glm::dvec3> rays;
    std::vector<size_t> resultIndices;
    std::vector<size_t> resultCounts;
  };
}

#endif // OHM_LINEKEYSQUERYDETAIL_H_
