#include "TreeSitterParser.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#if __has_include(<tree_sitter/api.h>)
#include <tree_sitter/api.h>
#define ULTRA_HAS_TREE_SITTER 1
#else
#define ULTRA_HAS_TREE_SITTER 0
struct TSParser;
struct TSTree;
struct TSNode;
#endif

#if ULTRA_HAS_TREE_SITTER
#include "ASTNormalizer.h"
#include "TreeSitterLanguages.h"
#endif

namespace ultra::ai::parsing {

namespace {

struct ScopeFrame {
  AstNodeKind kind{AstNodeKind::Root};
  std::string name;
  int depth{0};
};

constexpr std::size_t kNpos = std::string::npos;

bool isIdentifierStart(const char ch) {
  return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool isIdentifierPart(const char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' ||
         ch == ':' || ch == '.';
}

std::string trimCopy(const std::string& value) {
  std::size_t start = 0U;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  if (start >= value.size()) {
    return {};
  }

  std::size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool startsWithWord(const std::string& value, const std::string& word) {
  if (value.size() < word.size()) {
    return false;
  }
  if (value.compare(0U, word.size(), word) != 0) {
    return false;
  }
  if (value.size() == word.size()) {
    return true;
  }
  const char boundary = value[word.size()];
  return std::isspace(static_cast<unsigned char>(boundary)) != 0 ||
         boundary == '(' || boundary == ':' || boundary == '{';
}

std::size_t countChar(const std::string& value, const char needle) {
  return static_cast<std::size_t>(
      std::count(value.begin(), value.end(), needle));
}

std::string stripCppLikeComment(std::string line) {
  bool inSingleQuote = false;
  bool inDoubleQuote = false;
  for (std::size_t i = 0U; i + 1U < line.size(); ++i) {
    const char ch = line[i];
    if (ch == '\'' && !inDoubleQuote) {
      inSingleQuote = !inSingleQuote;
      continue;
    }
    if (ch == '"' && !inSingleQuote) {
      inDoubleQuote = !inDoubleQuote;
      continue;
    }
    if (!inSingleQuote && !inDoubleQuote && ch == '/' && line[i + 1U] == '/') {
      line.resize(i);
      break;
    }
  }
  return line;
}

std::string stripPythonComment(std::string line) {
  bool inSingleQuote = false;
  bool inDoubleQuote = false;
  for (std::size_t i = 0U; i < line.size(); ++i) {
    const char ch = line[i];
    if (ch == '\'' && !inDoubleQuote) {
      inSingleQuote = !inSingleQuote;
      continue;
    }
    if (ch == '"' && !inSingleQuote) {
      inDoubleQuote = !inDoubleQuote;
      continue;
    }
    if (!inSingleQuote && !inDoubleQuote && ch == '#') {
      line.resize(i);
      break;
    }
  }
  return line;
}

bool readLines(const std::filesystem::path& path,
               std::vector<std::string>& lines,
               bool keepEmpty = true) {
  lines.clear();
  std::ifstream input(path);
  if (!input) {
    return false;
  }
  std::string line;
  while (std::getline(input, line)) {
    if (!keepEmpty && trimCopy(line).empty()) {
      continue;
    }
    lines.push_back(std::move(line));
  }
  return true;
}

bool readFileToString(const std::filesystem::path& path, std::string& out) {
  out.clear();
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(input),
             std::istreambuf_iterator<char>());
  return true;
}

std::string extractIdentifierAfterKeyword(const std::string& line,
                                          const std::string& keyword) {
  const std::size_t pos = line.find(keyword);
  if (pos == kNpos) {
    return {};
  }
  std::size_t i = pos + keyword.size();
  while (i < line.size() &&
         std::isspace(static_cast<unsigned char>(line[i])) != 0) {
    ++i;
  }
  if (i >= line.size() || !isIdentifierStart(line[i])) {
    return {};
  }
  std::size_t j = i + 1U;
  while (j < line.size() && isIdentifierPart(line[j])) {
    ++j;
  }
  return line.substr(i, j - i);
}

std::string lastIdentifierBefore(const std::string& line, std::size_t beforePos) {
  if (beforePos == 0U || beforePos > line.size()) {
    return {};
  }
  std::size_t i = beforePos;
  while (i > 0U &&
         std::isspace(static_cast<unsigned char>(line[i - 1U])) != 0) {
    --i;
  }
  if (i == 0U) {
    return {};
  }

  std::size_t end = i;
  while (i > 0U && isIdentifierPart(line[i - 1U])) {
    --i;
  }
  if (i == end) {
    return {};
  }
  return line.substr(i, end - i);
}

std::string stripQualifiers(std::string symbol) {
  while (!symbol.empty() && (symbol.back() == ':' || symbol.back() == '.')) {
    symbol.pop_back();
  }
  const std::size_t nsPos = symbol.rfind("::");
  if (nsPos != kNpos) {
    symbol = symbol.substr(nsPos + 2U);
  }
  const std::size_t dotPos = symbol.rfind('.');
  if (dotPos != kNpos) {
    symbol = symbol.substr(dotPos + 1U);
  }
  return symbol;
}

bool isControlKeyword(const std::string& symbol) {
  static const std::set<std::string> kControlKeywords{
      "if",       "for",      "while",   "switch",  "catch",
      "return",   "sizeof",   "else",    "new",     "delete",
      "case",     "throw",    "try",     "do",      "break",
      "continue", "typename", "template"};
  return kControlKeywords.find(symbol) != kControlKeywords.end();
}

bool isBuiltinType(const std::string& token) {
  static const std::set<std::string> kBuiltinTypes{
      "auto",     "bool",      "char",       "double",   "float",
      "int",      "long",      "short",      "signed",   "unsigned",
      "void",     "size_t",    "std::size_t","uint8_t",  "uint16_t",
      "uint32_t", "uint64_t",  "int8_t",     "int16_t",  "int32_t",
      "int64_t",  "str",       "bytes",      "number",   "string"};
  return kBuiltinTypes.find(token) != kBuiltinTypes.end();
}

void addNode(AstNode& root, AstNode node) {
  root.children.push_back(std::move(node));
}

std::string currentClassLikeOwner(const std::vector<ScopeFrame>& scopes) {
  for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
    if (it->kind == AstNodeKind::ClassDecl ||
        it->kind == AstNodeKind::StructDecl ||
        it->kind == AstNodeKind::ReactComponentDecl) {
      return it->name;
    }
  }
  return {};
}

std::string currentFunctionOwner(const std::vector<ScopeFrame>& scopes) {
  for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
    if (it->kind == AstNodeKind::FunctionDecl ||
        it->kind == AstNodeKind::MethodDecl) {
      return it->name;
    }
  }
  return {};
}

std::string currentBestOwner(const std::vector<ScopeFrame>& scopes) {
  const std::string functionOwner = currentFunctionOwner(scopes);
  if (!functionOwner.empty()) {
    return functionOwner;
  }
  return currentClassLikeOwner(scopes);
}

std::string parseIncludeTarget(const std::string& line) {
  const std::size_t firstAngle = line.find('<');
  if (firstAngle != kNpos) {
    const std::size_t lastAngle = line.find('>', firstAngle + 1U);
    if (lastAngle != kNpos && lastAngle > firstAngle + 1U) {
      return line.substr(firstAngle + 1U, lastAngle - firstAngle - 1U);
    }
  }

  const std::size_t firstQuote = line.find('"');
  if (firstQuote != kNpos) {
    const std::size_t secondQuote = line.find('"', firstQuote + 1U);
    if (secondQuote != kNpos && secondQuote > firstQuote + 1U) {
      return line.substr(firstQuote + 1U, secondQuote - firstQuote - 1U);
    }
  }
  return {};
}

std::vector<std::string> splitCsv(const std::string& value) {
  std::vector<std::string> out;
  std::string current;
  int parenDepth = 0;
  int angleDepth = 0;
  for (const char ch : value) {
    if (ch == '(') {
      ++parenDepth;
    } else if (ch == ')' && parenDepth > 0) {
      --parenDepth;
    } else if (ch == '<') {
      ++angleDepth;
    } else if (ch == '>' && angleDepth > 0) {
      --angleDepth;
    }
    if (ch == ',' && parenDepth == 0 && angleDepth == 0) {
      out.push_back(trimCopy(current));
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  if (!trimCopy(current).empty()) {
    out.push_back(trimCopy(current));
  }
  return out;
}

std::vector<std::string> collectCallTargets(const std::string& line) {
  std::vector<std::string> out;
  std::set<std::string> dedupe;
  for (std::size_t i = 0U; i < line.size(); ++i) {
    if (line[i] != '(') {
      continue;
    }
    const std::string symbol = stripQualifiers(lastIdentifierBefore(line, i));
    if (symbol.empty() || isControlKeyword(symbol)) {
      continue;
    }
    if (dedupe.insert(symbol).second) {
      out.push_back(symbol);
    }
  }
  return out;
}

void pushScopeIfOpened(const AstNode& node,
                       const std::string& line,
                       int braceDepth,
                       std::vector<ScopeFrame>& scopes) {
  if (line.find('{') == kNpos) {
    return;
  }
  ScopeFrame frame;
  frame.kind = node.kind;
  frame.name = node.name;
  frame.depth = braceDepth + 1;
  scopes.push_back(std::move(frame));
}

void pruneScopesByDepth(int braceDepth, std::vector<ScopeFrame>& scopes) {
  while (!scopes.empty() && braceDepth < scopes.back().depth) {
    scopes.pop_back();
  }
}

void parseCpp(const std::vector<std::string>& lines, AstNode& root) {
  int braceDepth = 0;
  std::vector<ScopeFrame> scopes;

  for (std::size_t i = 0U; i < lines.size(); ++i) {
    const std::uint32_t lineNo = static_cast<std::uint32_t>(i + 1U);
    const std::string raw = stripCppLikeComment(lines[i]);
    const std::string line = trimCopy(raw);
    if (line.empty()) {
      continue;
    }

    if (startsWithWord(line, "#include")) {
      AstNode node;
      node.kind = AstNodeKind::IncludeDirective;
      node.value = parseIncludeTarget(line);
      node.startLine = lineNo;
      node.endLine = lineNo;
      addNode(root, std::move(node));
    }

    if (startsWithWord(line, "namespace")) {
      AstNode node;
      node.kind = AstNodeKind::NamespaceDecl;
      node.name = stripQualifiers(extractIdentifierAfterKeyword(line, "namespace"));
      node.signature = line;
      node.startLine = lineNo;
      node.endLine = lineNo;
      if (!node.name.empty()) {
        pushScopeIfOpened(node, line, braceDepth, scopes);
        addNode(root, std::move(node));
      }
    }

    std::string classKeyword;
    if (startsWithWord(line, "class")) {
      classKeyword = "class";
    } else if (startsWithWord(line, "struct")) {
      classKeyword = "struct";
    }
    if (!classKeyword.empty()) {
      AstNode node;
      node.kind =
          classKeyword == "class" ? AstNodeKind::ClassDecl : AstNodeKind::StructDecl;
      node.name = stripQualifiers(extractIdentifierAfterKeyword(line, classKeyword));
      node.signature = line;
      node.startLine = lineNo;
      node.endLine = lineNo;
      if (!node.name.empty()) {
        const std::size_t colonPos = line.find(':');
        if (colonPos != kNpos) {
          std::string inheritance = line.substr(colonPos + 1U);
          const std::size_t bracePos = inheritance.find('{');
          if (bracePos != kNpos) {
            inheritance = inheritance.substr(0U, bracePos);
          }
          const std::vector<std::string> bases = splitCsv(inheritance);
          for (std::string base : bases) {
            base = trimCopy(base);
            const std::string lowerBase = toLower(base);
            if (startsWithWord(lowerBase, "public")) {
              base = trimCopy(base.substr(6U));
            } else if (startsWithWord(lowerBase, "protected")) {
              base = trimCopy(base.substr(9U));
            } else if (startsWithWord(lowerBase, "private")) {
              base = trimCopy(base.substr(7U));
            }
            if (startsWithWord(toLower(base), "virtual")) {
              base = trimCopy(base.substr(7U));
            }
            base = stripQualifiers(base);
            if (base.empty()) {
              continue;
            }
            AstNode inheritanceNode;
            inheritanceNode.kind = AstNodeKind::InheritanceRef;
            inheritanceNode.name = base;
            inheritanceNode.owner = node.name;
            inheritanceNode.startLine = lineNo;
            inheritanceNode.endLine = lineNo;
            addNode(root, std::move(inheritanceNode));
          }
        }
        pushScopeIfOpened(node, line, braceDepth, scopes);
        addNode(root, std::move(node));
      }
    }

    std::string declaredFunctionName;
    const std::size_t openParenPos = line.find('(');
    if (openParenPos != kNpos) {
      const std::size_t closeParenPos = line.find(')', openParenPos + 1U);
      if (closeParenPos != kNpos) {
        const std::string candidateName =
            stripQualifiers(lastIdentifierBefore(line, openParenPos));
        const bool hasBodyAfterSignature =
            line.find('{', closeParenPos + 1U) != kNpos;
        if (hasBodyAfterSignature && !candidateName.empty() &&
            !isControlKeyword(candidateName) && candidateName != "operator") {
          declaredFunctionName = candidateName;
          AstNode node;
          const std::string classOwner = currentClassLikeOwner(scopes);
          node.kind =
              classOwner.empty() ? AstNodeKind::FunctionDecl : AstNodeKind::MethodDecl;
          node.name = declaredFunctionName;
          node.owner = classOwner;
          node.signature = line;
          node.startLine = lineNo;
          node.endLine = lineNo;
          pushScopeIfOpened(node, line, braceDepth, scopes);
          addNode(root, std::move(node));
        }
      }
    }

    if (line.back() == ';' && line.find('(') == kNpos && line.front() != '#') {
      const std::string lower = toLower(line);
      if (!startsWithWord(lower, "return") && !startsWithWord(lower, "using") &&
          !startsWithWord(lower, "typedef") && !startsWithWord(lower, "template") &&
          !startsWithWord(lower, "public") && !startsWithWord(lower, "private") &&
          !startsWithWord(lower, "protected") && !startsWithWord(lower, "case")) {
        const std::size_t assignmentPos = line.find('=');
        const std::size_t terminalPos =
            assignmentPos == kNpos ? line.size() - 1U : assignmentPos;
        std::string variable = stripQualifiers(lastIdentifierBefore(line, terminalPos));
        if (!variable.empty() && !isControlKeyword(variable)) {
          AstNode node;
          node.kind = AstNodeKind::VariableDecl;
          node.name = variable;
          node.owner = currentBestOwner(scopes);
          node.signature = line;
          node.startLine = lineNo;
          node.endLine = lineNo;
          addNode(root, std::move(node));

          std::size_t namePos = line.rfind(variable);
          if (namePos != kNpos && namePos > 0U) {
            std::string typeToken = trimCopy(line.substr(0U, namePos));
            const std::size_t spacePos = typeToken.find_last_of(" \t*&");
            if (spacePos != kNpos) {
              typeToken = trimCopy(typeToken.substr(spacePos + 1U));
            }
            typeToken = stripQualifiers(typeToken);
            if (!typeToken.empty() && !isBuiltinType(toLower(typeToken))) {
              AstNode typeNode;
              typeNode.kind = AstNodeKind::TypeRef;
              typeNode.name = typeToken;
              typeNode.owner = currentBestOwner(scopes);
              typeNode.startLine = lineNo;
              typeNode.endLine = lineNo;
              addNode(root, std::move(typeNode));
            }
          }
        }
      }
    }

    const std::vector<std::string> calls = collectCallTargets(line);
    for (const std::string& call : calls) {
      if (!declaredFunctionName.empty() && call == declaredFunctionName) {
        continue;
      }
      AstNode node;
      node.kind = AstNodeKind::CallExpr;
      node.name = call;
      node.owner = currentBestOwner(scopes);
      node.startLine = lineNo;
      node.endLine = lineNo;
      addNode(root, std::move(node));
    }

    const int delta = static_cast<int>(countChar(raw, '{')) -
                      static_cast<int>(countChar(raw, '}'));
    braceDepth += delta;
    pruneScopesByDepth(braceDepth, scopes);
  }
}

std::string parseJavaScriptImportTarget(const std::string& line) {
  const std::size_t fromPos = line.find(" from ");
  if (fromPos != kNpos) {
    const std::size_t q1 = line.find_first_of("'\"", fromPos + 6U);
    if (q1 != kNpos) {
      const std::size_t q2 = line.find(line[q1], q1 + 1U);
      if (q2 != kNpos && q2 > q1 + 1U) {
        return line.substr(q1 + 1U, q2 - q1 - 1U);
      }
    }
  }
  const std::size_t q1 = line.find_first_of("'\"");
  if (q1 != kNpos) {
    const std::size_t q2 = line.find(line[q1], q1 + 1U);
    if (q2 != kNpos && q2 > q1 + 1U) {
      return line.substr(q1 + 1U, q2 - q1 - 1U);
    }
  }
  return {};
}

std::string parseRequireTarget(const std::string& line) {
  const std::size_t reqPos = line.find("require");
  if (reqPos == kNpos) {
    return {};
  }
  const std::size_t open = line.find('(', reqPos);
  if (open == kNpos) {
    return {};
  }
  const std::size_t q1 = line.find_first_of("'\"", open + 1U);
  if (q1 == kNpos) {
    return {};
  }
  const std::size_t q2 = line.find(line[q1], q1 + 1U);
  if (q2 == kNpos || q2 <= q1 + 1U) {
    return {};
  }
  return line.substr(q1 + 1U, q2 - q1 - 1U);
}

bool isJavaScriptLikeMethodLine(const std::string& line) {
  if (line.find('(') == kNpos || line.find(')') == kNpos || line.find('{') == kNpos) {
    return false;
  }
  const std::size_t parenPos = line.find('(');
  const std::string name = stripQualifiers(lastIdentifierBefore(line, parenPos));
  if (name.empty()) {
    return false;
  }
  const std::string lower = toLower(name);
  return lower != "if" && lower != "for" && lower != "while" &&
         lower != "switch" && lower != "catch" && lower != "function";
}

void parseJavaScriptLike(const std::vector<std::string>& lines, AstNode& root) {
  int braceDepth = 0;
  std::vector<ScopeFrame> scopes;

  for (std::size_t i = 0U; i < lines.size(); ++i) {
    const std::uint32_t lineNo = static_cast<std::uint32_t>(i + 1U);
    const std::string raw = stripCppLikeComment(lines[i]);
    const std::string line = trimCopy(raw);
    if (line.empty()) {
      continue;
    }

    if (startsWithWord(line, "import")) {
      AstNode importNode;
      importNode.kind = AstNodeKind::ImportStatement;
      importNode.value = parseJavaScriptImportTarget(line);
      importNode.startLine = lineNo;
      importNode.endLine = lineNo;
      if (!importNode.value.empty()) {
        addNode(root, std::move(importNode));
      }
    }

    if (line.find("require(") != kNpos) {
      AstNode importNode;
      importNode.kind = AstNodeKind::ImportStatement;
      importNode.value = parseRequireTarget(line);
      importNode.startLine = lineNo;
      importNode.endLine = lineNo;
      if (!importNode.value.empty()) {
        addNode(root, std::move(importNode));
      }
    }

    if (startsWithWord(line, "export")) {
      AstNode exportNode;
      exportNode.kind = AstNodeKind::ExportStatement;
      exportNode.startLine = lineNo;
      exportNode.endLine = lineNo;
      exportNode.signature = line;
      if (line.find("export default") != kNpos) {
        const std::size_t fnPos = line.find("function");
        const std::size_t clsPos = line.find("class");
        if (fnPos != kNpos) {
          exportNode.name = extractIdentifierAfterKeyword(line, "function");
          if (exportNode.name.empty()) {
            exportNode.name = "default";
          }
        } else if (clsPos != kNpos) {
          exportNode.name = extractIdentifierAfterKeyword(line, "class");
          if (exportNode.name.empty()) {
            exportNode.name = "default";
          }
        } else {
          exportNode.name = "default";
        }
      } else {
        std::string named = extractIdentifierAfterKeyword(line, "const");
        if (named.empty()) {
          named = extractIdentifierAfterKeyword(line, "function");
        }
        if (named.empty()) {
          named = extractIdentifierAfterKeyword(line, "class");
        }
        exportNode.name = named.empty() ? "export" : named;
      }
      addNode(root, std::move(exportNode));
    }

    if (line.find("class ") != kNpos) {
      AstNode classNode;
      classNode.kind = AstNodeKind::ClassDecl;
      classNode.name = stripQualifiers(extractIdentifierAfterKeyword(line, "class"));
      classNode.signature = line;
      classNode.startLine = lineNo;
      classNode.endLine = lineNo;
      if (!classNode.name.empty()) {
        const std::size_t extendsPos = line.find("extends ");
        if (extendsPos != kNpos) {
          std::string base =
              trimCopy(line.substr(extendsPos + std::string("extends ").size()));
          const std::size_t bracePos = base.find('{');
          if (bracePos != kNpos) {
            base = trimCopy(base.substr(0U, bracePos));
          }
          base = stripQualifiers(base);
          if (!base.empty()) {
            AstNode inheritNode;
            inheritNode.kind = AstNodeKind::InheritanceRef;
            inheritNode.name = base;
            inheritNode.owner = classNode.name;
            inheritNode.startLine = lineNo;
            inheritNode.endLine = lineNo;
            addNode(root, std::move(inheritNode));
          }
          if (base == "Component" || base == "ReactComponent" ||
              base == "React.Component") {
            AstNode reactNode;
            reactNode.kind = AstNodeKind::ReactComponentDecl;
            reactNode.name = classNode.name;
            reactNode.owner = classNode.name;
            reactNode.startLine = lineNo;
            reactNode.endLine = lineNo;
            reactNode.signature = line;
            addNode(root, std::move(reactNode));
          }
        }
        pushScopeIfOpened(classNode, line, braceDepth, scopes);
        addNode(root, std::move(classNode));
      }
    }

    std::string declaredFunctionName;
    if (line.find("function ") != kNpos) {
      declaredFunctionName =
          stripQualifiers(extractIdentifierAfterKeyword(line, "function"));
    } else if ((startsWithWord(line, "const") || startsWithWord(line, "let") ||
                startsWithWord(line, "var")) &&
               line.find("=>") != kNpos) {
      const std::size_t assignPos = line.find('=');
      declaredFunctionName =
          stripQualifiers(lastIdentifierBefore(line, assignPos == kNpos ? line.size() : assignPos));
    } else if (!currentClassLikeOwner(scopes).empty() &&
               isJavaScriptLikeMethodLine(line)) {
      const std::size_t parenPos = line.find('(');
      declaredFunctionName = stripQualifiers(lastIdentifierBefore(line, parenPos));
    }

    if (!declaredFunctionName.empty() &&
        !isControlKeyword(toLower(declaredFunctionName))) {
      AstNode fnNode;
      const std::string classOwner = currentClassLikeOwner(scopes);
      fnNode.kind = classOwner.empty() ? AstNodeKind::FunctionDecl : AstNodeKind::MethodDecl;
      fnNode.name = declaredFunctionName;
      fnNode.owner = classOwner;
      fnNode.signature = line;
      fnNode.startLine = lineNo;
      fnNode.endLine = lineNo;
      if (!declaredFunctionName.empty() &&
          std::isupper(static_cast<unsigned char>(declaredFunctionName[0])) != 0) {
        AstNode reactNode;
        reactNode.kind = AstNodeKind::ReactComponentDecl;
        reactNode.name = declaredFunctionName;
        reactNode.owner = declaredFunctionName;
        reactNode.startLine = lineNo;
        reactNode.endLine = lineNo;
        reactNode.signature = line;
        addNode(root, std::move(reactNode));
      }
      pushScopeIfOpened(fnNode, line, braceDepth, scopes);
      addNode(root, std::move(fnNode));
    }

    if ((startsWithWord(line, "const") || startsWithWord(line, "let") ||
         startsWithWord(line, "var")) &&
        line.find('=') != kNpos) {
      const std::size_t assignPos = line.find('=');
      std::string variableName = stripQualifiers(lastIdentifierBefore(line, assignPos));
      if (!variableName.empty()) {
        AstNode variableNode;
        variableNode.kind = AstNodeKind::VariableDecl;
        variableNode.name = variableName;
        variableNode.owner = currentBestOwner(scopes);
        variableNode.signature = line;
        variableNode.startLine = lineNo;
        variableNode.endLine = lineNo;
        addNode(root, std::move(variableNode));
      }
    }

    const std::vector<std::string> calls = collectCallTargets(line);
    for (const std::string& call : calls) {
      if (!declaredFunctionName.empty() && call == declaredFunctionName) {
        continue;
      }
      AstNode callNode;
      callNode.kind = AstNodeKind::CallExpr;
      callNode.name = call;
      callNode.owner = currentBestOwner(scopes);
      callNode.startLine = lineNo;
      callNode.endLine = lineNo;
      addNode(root, std::move(callNode));
    }

    const int delta = static_cast<int>(countChar(raw, '{')) -
                      static_cast<int>(countChar(raw, '}'));
    braceDepth += delta;
    pruneScopesByDepth(braceDepth, scopes);
  }
}

int pythonIndentWidth(const std::string& line) {
  int count = 0;
  for (const char ch : line) {
    if (ch == ' ') {
      ++count;
      continue;
    }
    if (ch == '\t') {
      count += 4;
      continue;
    }
    break;
  }
  return count;
}

void parsePython(const std::vector<std::string>& lines, AstNode& root) {
  std::vector<ScopeFrame> scopes;

  for (std::size_t i = 0U; i < lines.size(); ++i) {
    const std::uint32_t lineNo = static_cast<std::uint32_t>(i + 1U);
    const std::string raw = stripPythonComment(lines[i]);
    const std::string line = trimCopy(raw);
    if (line.empty()) {
      continue;
    }
    const int indent = pythonIndentWidth(raw);

    while (!scopes.empty() && indent <= scopes.back().depth) {
      scopes.pop_back();
    }

    if (startsWithWord(line, "import")) {
      std::string target = trimCopy(line.substr(std::string("import").size()));
      const std::size_t comma = target.find(',');
      if (comma != kNpos) {
        target = trimCopy(target.substr(0U, comma));
      }
      AstNode importNode;
      importNode.kind = AstNodeKind::ImportStatement;
      importNode.value = target;
      importNode.startLine = lineNo;
      importNode.endLine = lineNo;
      if (!importNode.value.empty()) {
        addNode(root, std::move(importNode));
      }
    } else if (startsWithWord(line, "from")) {
      std::string target =
          trimCopy(line.substr(std::string("from").size()));
      const std::size_t importPos = target.find(" import ");
      if (importPos != kNpos) {
        target = trimCopy(target.substr(0U, importPos));
      }
      AstNode importNode;
      importNode.kind = AstNodeKind::ImportStatement;
      importNode.value = target;
      importNode.startLine = lineNo;
      importNode.endLine = lineNo;
      if (!importNode.value.empty()) {
        addNode(root, std::move(importNode));
      }
    }

    if (startsWithWord(line, "class")) {
      AstNode classNode;
      classNode.kind = AstNodeKind::ClassDecl;
      classNode.name = stripQualifiers(extractIdentifierAfterKeyword(line, "class"));
      classNode.signature = line;
      classNode.startLine = lineNo;
      classNode.endLine = lineNo;
      if (!classNode.name.empty()) {
        const std::size_t openParen = line.find('(');
        const std::size_t closeParen = line.find(')', openParen + 1U);
        if (openParen != kNpos && closeParen != kNpos && closeParen > openParen + 1U) {
          const std::string basesSegment =
              line.substr(openParen + 1U, closeParen - openParen - 1U);
          const std::vector<std::string> bases = splitCsv(basesSegment);
          for (std::string base : bases) {
            base = stripQualifiers(trimCopy(base));
            if (base.empty()) {
              continue;
            }
            AstNode inheritNode;
            inheritNode.kind = AstNodeKind::InheritanceRef;
            inheritNode.name = base;
            inheritNode.owner = classNode.name;
            inheritNode.startLine = lineNo;
            inheritNode.endLine = lineNo;
            addNode(root, std::move(inheritNode));
          }
        }
        ScopeFrame scope;
        scope.kind = AstNodeKind::ClassDecl;
        scope.name = classNode.name;
        scope.depth = indent;
        scopes.push_back(std::move(scope));
        addNode(root, std::move(classNode));
      }
    }

    if (startsWithWord(line, "def")) {
      AstNode fnNode;
      fnNode.name = stripQualifiers(extractIdentifierAfterKeyword(line, "def"));
      fnNode.signature = line;
      fnNode.startLine = lineNo;
      fnNode.endLine = lineNo;
      const std::string classOwner = currentClassLikeOwner(scopes);
      fnNode.kind = classOwner.empty() ? AstNodeKind::FunctionDecl : AstNodeKind::MethodDecl;
      fnNode.owner = classOwner;
      if (!fnNode.name.empty()) {
        ScopeFrame scope;
        scope.kind = fnNode.kind;
        scope.name = fnNode.name;
        scope.depth = indent;
        scopes.push_back(std::move(scope));
        addNode(root, std::move(fnNode));
      }
    }

    const std::size_t assignPos = line.find('=');
    if (assignPos != kNpos && line.find("==") == kNpos &&
        !startsWithWord(line, "if") && !startsWithWord(line, "while") &&
        !startsWithWord(line, "return")) {
      const std::string variableName =
          stripQualifiers(lastIdentifierBefore(line, assignPos));
      if (!variableName.empty()) {
        AstNode varNode;
        varNode.kind = AstNodeKind::VariableDecl;
        varNode.name = variableName;
        varNode.owner = currentBestOwner(scopes);
        varNode.signature = line;
        varNode.startLine = lineNo;
        varNode.endLine = lineNo;
        addNode(root, std::move(varNode));
      }
    }

    const std::vector<std::string> calls = collectCallTargets(line);
    for (const std::string& call : calls) {
      if (startsWithWord(line, "def") && call == extractIdentifierAfterKeyword(line, "def")) {
        continue;
      }
      AstNode callNode;
      callNode.kind = AstNodeKind::CallExpr;
      callNode.name = call;
      callNode.owner = currentBestOwner(scopes);
      callNode.startLine = lineNo;
      callNode.endLine = lineNo;
      addNode(root, std::move(callNode));
    }
  }
}

void sortAstDeterministic(AstNode& root) {
  std::sort(root.children.begin(), root.children.end(),
            [](const AstNode& left, const AstNode& right) {
              if (left.startLine != right.startLine) {
                return left.startLine < right.startLine;
              }
              if (left.kind != right.kind) {
                return static_cast<std::uint8_t>(left.kind) <
                       static_cast<std::uint8_t>(right.kind);
              }
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return left.value < right.value;
            });
}

bool parseWithTreeSitter(const std::filesystem::path& path,
                         const Language language,
                         ParsedAST& ast) {
#if ULTRA_HAS_TREE_SITTER
  TSParser* parser = ts_parser_new();
  if (parser == nullptr) {
    return false;
  }

  const TSLanguage* tsLang = TreeSitterLanguages::getTreeSitterLanguage(language);
  if (tsLang == nullptr) {
    ts_parser_delete(parser);
    return false;
  }

  if (!ts_parser_set_language(parser, tsLang)) {
    ts_parser_delete(parser);
    return false;
  }

  std::string source;
  if (!readFileToString(path, source)) {
    ts_parser_delete(parser);
    return false;
  }

  TSTree* tree = ts_parser_parse_string(parser, nullptr, source.c_str(),
                                        static_cast<uint32_t>(source.size()));
  if (tree == nullptr) {
    ts_parser_delete(parser);
    return false;
  }

  TSNode rootNode = ts_tree_root_node(tree);
  ASTNormalizer::walkTree(rootNode, source, ast.root);

  ts_tree_delete(tree);
  ts_parser_delete(parser);
  return true;
#else
  (void)path;
  (void)language;
  (void)ast;
  return false;
#endif
}

}  // namespace

bool TreeSitterParser::parseFile(const std::filesystem::path& path,
                                 const Language language,
                                 ParsedAST& ast) const {
  ast = ParsedAST{};
  ast.language = language;
  ast.sourcePath = path;
  ast.root.kind = AstNodeKind::Root;
  ast.usedTreeSitterBackend = false;

#if ULTRA_HAS_TREE_SITTER
  if (parseWithTreeSitter(path, language, ast)) {
    ast.usedTreeSitterBackend = true;
    sortAstDeterministic(ast.root);
    ast.valid = true;
    return true;
  }
#endif

  std::vector<std::string> lines;
  if (!readLines(path, lines)) {
    return false;
  }

  switch (language) {
    case Language::Cpp:
      parseCpp(lines, ast.root);
      break;
    case Language::JavaScript:
    case Language::TypeScript:
      parseJavaScriptLike(lines, ast.root);
      break;
    case Language::Python:
      parsePython(lines, ast.root);
      break;
    default:
      return false;
  }

  sortAstDeterministic(ast.root);
  ast.valid = true;
  return true;
}

}  // namespace ultra::ai::parsing
