#pragma once

#include <string>
#include <vector>

#include "endstone_exchange/core/Models.hpp"

namespace exchange::catalog {

struct CatalogCategory {
    std::string id;
    std::string name;
    std::string icon;
};

const std::vector<CatalogCategory>& categories();
const std::vector<Product>& products();

}  // namespace exchange::catalog
