#include "SymbolExtractor.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <unordered_map>

namespace ultra::ai::parsing {

namespace {

Visibility visibilityForSymbol(const AstNode& node) {
  if (node.kind == AstNodeKind::ImportStatement ||
      node.kind == AstNodeKind::ExportStatement) {
    return Visibility::Module;
  }
  if (!node.name.empty() && node.name[0] == '_') {
    return Visibility::Private;
  }
  return Visibility::Public;
}

static std::string stripScope(const std::string& name) {
  const size_t pos = name.rfind("::");
  if (pos != std::string::npos) {
    return name.substr(pos + 2);
  }
  return name;
}

void maybeAppendSymbol(const AstNode& node,
                       std::vector<ExtractedSymbol>& symbols,
                       std::set<std::tuple<std::uint32_t, std::string,
                                           std::uint8_t, std::uint8_t>>& dedupe) {

  ExtractedSymbol symbol;
  bool include = true;

  switch (node.kind) {

    case AstNodeKind::ClassDecl:
    case AstNodeKind::StructDecl:
      symbol.symbolType = SymbolType::Class;
      symbol.name = node.name;
      break;

    case AstNodeKind::FunctionDecl:
    case AstNodeKind::MethodDecl:
      symbol.symbolType =
          node.name == "main" ? SymbolType::EntryPoint : SymbolType::Function;
      symbol.name = node.name;
      break;

    case AstNodeKind::ReactComponentDecl:
      symbol.symbolType = SymbolType::ReactComponent;
      symbol.name = node.name;
      break;

    case AstNodeKind::ImportStatement:
    case AstNodeKind::IncludeDirective:
      symbol.symbolType = SymbolType::Import;
      symbol.name = node.value.empty() ? node.name : node.value;
      break;

    case AstNodeKind::ExportStatement:
      symbol.symbolType = SymbolType::Export;
      symbol.name = node.name;
      break;

    case AstNodeKind::NamespaceDecl:
    case AstNodeKind::VariableDecl:
      symbol.symbolType = SymbolType::Unknown;
      symbol.name = node.name;
      break;

    default:
      include = false;
      break;
  }

  if (!include || symbol.name.empty()) {
    return;
  }

  symbol.signature = node.signature;
  symbol.visibility = visibilityForSymbol(node);
  symbol.lineNumber = node.startLine;

  const auto key = std::make_tuple(
      symbol.lineNumber,
      symbol.name,
      static_cast<std::uint8_t>(symbol.symbolType),
      static_cast<std::uint8_t>(symbol.visibility));

  if (!dedupe.insert(key).second) {
    return;
  }

  symbols.push_back(std::move(symbol));
}

} // namespace


std::vector<ExtractedSymbol> SymbolExtractor::extract(const ParsedAST& ast) {

  std::vector<ExtractedSymbol> symbols;

  if (!ast.valid) {
    return symbols;
  }

  std::set<std::tuple<std::uint32_t,std::string,std::uint8_t,std::uint8_t>> dedupe;

  std::unordered_map<std::string,size_t> symbolMap;
  symbolMap.reserve(ast.root.children.size());

  // PASS 1: collect symbols
  for (const AstNode& node : ast.root.children) {

    const auto before = symbols.size();

    maybeAppendSymbol(node, symbols, dedupe);

    if (symbols.size() != before) {

      ExtractedSymbol& sym = symbols.back();

      // avoid overwriting earlier symbols
      symbolMap.emplace(sym.name, symbols.size() - 1);
    }
  }

  // helper: nearest symbol fallback
  auto findNearestSymbol = [&](uint32_t line) -> ExtractedSymbol* {

    ExtractedSymbol* best = nullptr;

    for (auto& s : symbols) {

      if (s.lineNumber <= line) {
        best = &s;
      }
    }

    return best;
  };


  // PASS 2: attach relationships
  for (const AstNode& node : ast.root.children) {

    if (node.name.empty()) {
      continue;
    }

    ExtractedSymbol* ownerSymbol = nullptr;

    if (!node.owner.empty()) {

      std::string owner = stripScope(node.owner);

      auto it = symbolMap.find(owner);

      if (it != symbolMap.end()) {
        ownerSymbol = &symbols[it->second];
      }
    }

    if (!ownerSymbol) {
      ownerSymbol = findNearestSymbol(node.startLine);
    }

    if (!ownerSymbol) {
      continue;
    }

    if (node.kind == AstNodeKind::CallExpr) {
      ownerSymbol->calls.push_back(node.name);
    }
    else if (node.kind == AstNodeKind::InheritanceRef) {
      ownerSymbol->bases.push_back(node.name);
    }
    else if (node.kind == AstNodeKind::TypeRef) {
      ownerSymbol->typeRefs.push_back(node.name);
    }
  }


  auto dedup = [](std::vector<std::string>& v) {

    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
  };

  for (auto& s : symbols) {

    dedup(s.calls);
    dedup(s.bases);
    dedup(s.typeRefs);
  }


  std::sort(symbols.begin(), symbols.end(),
      [](const ExtractedSymbol& a, const ExtractedSymbol& b) {

        if (a.lineNumber != b.lineNumber)
          return a.lineNumber < b.lineNumber;

        if (a.name != b.name)
          return a.name < b.name;

        if (a.symbolType != b.symbolType)
          return static_cast<uint8_t>(a.symbolType) <
                 static_cast<uint8_t>(b.symbolType);

        return static_cast<uint8_t>(a.visibility) <
               static_cast<uint8_t>(b.visibility);
      });

  return symbols;
}

} // namespace ultra::ai::parsing