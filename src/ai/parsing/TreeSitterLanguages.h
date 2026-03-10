#pragma once

#include "../FileRegistry.h"

#include <tree_sitter/api.h>

namespace ultra::ai::parsing::TreeSitterLanguages {

const TSLanguage* getTreeSitterLanguage(Language lang);

}  // namespace ultra::ai::parsing::TreeSitterLanguages
