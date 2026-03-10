// Compiled only when ULTRA_USE_LIBCLANG is defined and LibClang is linked.
#include "ast/AstTypes.h"
#include "ast/AstExtractor.h"
#include <clang-c/Index.h>
#include <filesystem>
#include <memory>
#include <string>

namespace ultra::ast {

namespace {

std::string cxStringToStd(CXString s) {
  if (s.data == nullptr) return {};
  std::string out(clang_getCString(s));
  clang_disposeString(s);
  return out;
}

unsigned getLine(CXCursor cursor) {
  CXSourceLocation loc = clang_getCursorLocation(cursor);
  unsigned line = 0, column = 0;
  clang_getSpellingLocation(loc, nullptr, &line, &column, nullptr);
  return line;
}

FileStructure g_result;
std::filesystem::path g_mainFile;

enum CXChildVisitResult visitCursor(CXCursor cursor, CXCursor /*parent*/,
                                    CXClientData /*data*/) {
  CXCursorKind kind = clang_getCursorKind(cursor);
  std::string name = cxStringToStd(clang_getCursorSpelling(cursor));
  unsigned line = getLine(cursor);

  switch (kind) {
    case CXCursor_ClassDecl:
      if (!name.empty())
        g_result.classes.push_back({name, static_cast<std::size_t>(line)});
      break;
    case CXCursor_StructDecl:
      if (!name.empty())
        g_result.structs.push_back({name, static_cast<std::size_t>(line)});
      break;
    case CXCursor_Namespace:
      if (!name.empty())
        g_result.namespaces.push_back({name, static_cast<std::size_t>(line)});
      break;
    case CXCursor_CXXMethod:
      if (!name.empty())
        g_result.methods.push_back({name + "()", static_cast<std::size_t>(line)});
      break;
    case CXCursor_FunctionDecl:
      if (!name.empty()) {
        CXSourceLocation loc = clang_getCursorLocation(cursor);
        CXFile file;
        unsigned l = 0;
        clang_getSpellingLocation(loc, &file, &l, nullptr, nullptr);
        std::string fileName = cxStringToStd(clang_getFileName(file));
        if (fileName == g_mainFile.string())
          g_result.freeFunctions.push_back(
              {name + "()", static_cast<std::size_t>(line)});
      }
      break;
    default:
      break;
  }
  return CXChildVisit_Recurse;
}

}  // namespace

FileStructure extractWithLibClang(const std::filesystem::path& file) {
  g_result = FileStructure{};
  g_mainFile = std::filesystem::absolute(file).lexically_normal();

  std::string pathStr = g_mainFile.string();
  const char* args[] = {"-std=c++17", "-Wno-unknown-pragmas"};
  CXIndex index = clang_createIndex(0, 0);
  CXTranslationUnit tu = clang_parseTranslationUnit(
      index, pathStr.c_str(), args, 2,
      nullptr, 0, CXTranslationUnit_None);
  if (!tu) {
    clang_disposeIndex(index);
    return FileStructure{};
  }

  CXCursor root = clang_getTranslationUnitCursor(tu);
  clang_visitChildren(root, visitCursor, nullptr);

  clang_disposeTranslationUnit(tu);
  clang_disposeIndex(index);
  return g_result;
}

}  // namespace ultra::ast
