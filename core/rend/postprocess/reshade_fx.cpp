/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "reshade_fx.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace postprocess {
namespace {

enum class TokenKind { Identifier, Number, String, Symbol, Directive, End };

struct Token {
	TokenKind kind;
	std::string text;
	size_t begin;
	size_t end;
	size_t line;
};

bool isIdentifierStart(char c)
{
	return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool isIdentifierPart(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string lowercase(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

bool isIncludeDirective(const std::string& directive)
{
	size_t index = 0;
	while (index < directive.size() && (directive[index] == '#' || std::isspace(static_cast<unsigned char>(directive[index]))))
		++index;
	return lowercase(directive.substr(index, 7)) == "include";
}

std::string trim(const std::string& value)
{
	const size_t first = value.find_first_not_of(" \t\r");
	if (first == std::string::npos)
		return {};
	const size_t last = value.find_last_not_of(" \t\r");
	return value.substr(first, last - first + 1);
}

bool evaluateCondition(const std::string& expression, const std::unordered_map<std::string, std::string>& defines,
		bool& result)
{
	std::function<bool(std::string, bool&)> parse = [&](std::string value, bool& parsed) -> bool {
		value = trim(value);
		while (value.size() >= 2 && value.front() == '(' && value.back() == ')') {
			int depth = 0;
			bool encloses = true;
			for (size_t index = 0; index < value.size(); ++index) {
				if (value[index] == '(') ++depth;
				else if (value[index] == ')' && --depth == 0 && index + 1 != value.size()) { encloses = false; break; }
			}
			if (!encloses) break;
			value = trim(value.substr(1, value.size() - 2));
		}
		auto split = [&](const char *op) -> size_t {
			int depth = 0;
			for (size_t index = 0; index + 1 < value.size(); ++index) {
				if (value[index] == '(') ++depth;
				else if (value[index] == ')') --depth;
				if (depth == 0 && value.compare(index, 2, op) == 0) return index;
			}
			return std::string::npos;
		};
		if (const size_t splitAt = split("||"); splitAt != std::string::npos) {
			bool left = false, right = false;
			if (!parse(value.substr(0, splitAt), left) || !parse(value.substr(splitAt + 2), right)) return false;
			parsed = left || right;
			return true;
		}
		if (const size_t splitAt = split("&&"); splitAt != std::string::npos) {
			bool left = false, right = false;
			if (!parse(value.substr(0, splitAt), left) || !parse(value.substr(splitAt + 2), right)) return false;
			parsed = left && right;
			return true;
		}
		if (!value.empty() && value.front() == '!') {
			if (!parse(value.substr(1), parsed)) return false;
			parsed = !parsed;
			return true;
		}
		const std::regex comparison(R"(^(.*?)(==|!=|<=|>=|<|>)(.*?)$)");
		std::smatch match;
		if (std::regex_match(value, match, comparison)) {
			const std::string leftText = trim(match[1].str());
			const std::string rightText = trim(match[3].str());
		auto numericValue = [&](const std::string& text, long long& number) {
			const auto define = defines.find(text);
			const std::string value = define == defines.end() ? text : define->second;
			char *end = nullptr;
			number = std::strtoll(value.c_str(), &end, 0);
			return end != value.c_str() && *end == '\0';
		};
		long long left = 0;
		long long right = 0;
		if (!numericValue(leftText, left) || !numericValue(rightText, right)) return false;
			const std::string op = match[2].str();
			parsed = op == "==" ? left == right : op == "!=" ? left != right : op == "<=" ? left <= right
					: op == ">=" ? left >= right : op == "<" ? left < right : left > right;
			return true;
		}
		if (value.rfind("defined", 0) == 0) {
			const size_t open = value.find('(');
			const size_t close = value.rfind(')');
			const std::string name = open != std::string::npos && close > open ? trim(value.substr(open + 1, close - open - 1)) : trim(value.substr(7));
			if (name.empty() || (open != std::string::npos && close != value.size() - 1)) return false;
			parsed = defines.count(name) != 0;
			return true;
		}
		const auto define = defines.find(value);
		const std::string numeric = define == defines.end() ? value : define->second;
		char *end = nullptr;
		const long long number = std::strtoll(numeric.c_str(), &end, 0);
		if (end == numeric.c_str() || *end != '\0') return false;
		parsed = number != 0;
		return true;
	};
	return parse(expression, result);
}

void expandObjectMacros(std::string& line, const std::unordered_map<std::string, std::string>& defines)
{
	for (unsigned pass = 0; pass < 8; ++pass) {
		std::string expanded;
		bool changed = false;
		for (size_t index = 0; index < line.size();) {
			if (!std::isalpha(static_cast<unsigned char>(line[index])) && line[index] != '_') {
				expanded += line[index++];
				continue;
			}
			const size_t begin = index++;
			while (index < line.size() && (std::isalnum(static_cast<unsigned char>(line[index])) || line[index] == '_'))
				++index;
			const std::string name = line.substr(begin, index - begin);
			const auto define = defines.find(name);
			if (define == defines.end() || define->second.empty() || define->second == name)
				expanded += name;
			else {
				expanded += define->second;
				changed = true;
			}
		}
		line = std::move(expanded);
		if (!changed)
			break;
	}
}

struct FunctionMacro {
	std::vector<std::string> arguments;
	std::string body;
};

void expandFunctionMacros(std::string& line, const std::unordered_map<std::string, FunctionMacro>& defines)
{
	for (unsigned pass = 0; pass < 8; ++pass) {
		bool changed = false;
		for (const auto& [name, macro] : defines) {
			size_t search = 0;
			while ((search = line.find(name, search)) != std::string::npos) {
				const bool leftBoundary = search == 0 || (!std::isalnum(static_cast<unsigned char>(line[search - 1])) && line[search - 1] != '_');
				const size_t open = line.find_first_not_of(" \t", search + name.size());
				if (!leftBoundary || open == std::string::npos || line[open] != '(') {
					search += name.size();
					continue;
				}
				int depth = 1;
				size_t close = open + 1;
				for (; close < line.size() && depth != 0; ++close) {
					if (line[close] == '(') ++depth;
					else if (line[close] == ')') --depth;
				}
				if (depth != 0) break;
				std::vector<std::string> arguments;
				size_t begin = open + 1;
				depth = 0;
				for (size_t index = begin; index <= close; ++index) {
					const char character = index < close ? line[index] : ',';
					if (character == '(') ++depth;
					else if (character == ')') --depth;
					if (character == ',' && depth == 0) {
						arguments.push_back(trim(line.substr(begin, index - begin)));
						begin = index + 1;
					}
				}
				if (arguments.size() == 1 && arguments[0].empty()) arguments.clear();
				if (arguments.size() != macro.arguments.size()) {
					search = close + 1;
					continue;
				}
				std::string replacement = macro.body;
				for (size_t argument = 0; argument < macro.arguments.size(); ++argument) {
					const std::regex parameter("\\b" + macro.arguments[argument] + "\\b");
					replacement = std::regex_replace(replacement, parameter, arguments[argument]);
				}
				line.replace(search, close + 1 - search, replacement);
				search += replacement.size();
				changed = true;
			}
		}
		if (!changed) break;
	}
}

bool preprocess(const std::string& source, std::string& output, std::string& error)
{
	struct Conditional {
		bool parentActive;
		bool branchTaken;
		bool active;
	};
	std::unordered_map<std::string, std::string> defines;
	std::unordered_map<std::string, FunctionMacro> functionDefines;
	std::vector<Conditional> conditionals;
	auto active = [&] { return conditionals.empty() || conditionals.back().active; };
	std::istringstream lines(source);
	std::string line;
	output.clear();
	size_t lineNumber = 0;
	while (std::getline(lines, line)) {
		++lineNumber;
		const std::string stripped = trim(line);
		if (stripped.empty() || stripped.front() != '#') {
			if (active()) {
				expandFunctionMacros(line, functionDefines);
				expandObjectMacros(line, defines);
				expandFunctionMacros(line, functionDefines);
				output += line + '\n';
			}
			continue;
		}
		std::istringstream directive(stripped.substr(1));
		std::string command;
		directive >> command;
		std::string argument;
		std::getline(directive, argument);
		argument = trim(argument);
		if (lowercase(command) == "include") {
			error = "line " + std::to_string(lineNumber) + ": includes are not permitted in ReShade FX effects.";
			return false;
		}
		if (command == "if" || command == "ifdef" || command == "ifndef") {
			bool condition = false;
			if (command == "ifdef") condition = defines.count(argument) != 0;
			else if (command == "ifndef") condition = defines.count(argument) == 0;
			else if (!evaluateCondition(argument, defines, condition)) {
				error = "line " + std::to_string(lineNumber) + ": unsupported ReShade FX #if condition.";
				return false;
			}
			const bool parentActive = active();
			conditionals.push_back({ parentActive, condition, parentActive && condition });
			continue;
		}
		if (command == "elif") {
			if (conditionals.empty()) {
				error = "line " + std::to_string(lineNumber) + ": #elif without #if.";
				return false;
			}
			Conditional& conditional = conditionals.back();
			bool condition = false;
			if (!evaluateCondition(argument, defines, condition)) {
				error = "line " + std::to_string(lineNumber) + ": unsupported ReShade FX #elif condition.";
				return false;
			}
			conditional.active = conditional.parentActive && !conditional.branchTaken && condition;
			conditional.branchTaken = conditional.branchTaken || condition;
			continue;
		}
		if (command == "else") {
			if (conditionals.empty()) {
				error = "line " + std::to_string(lineNumber) + ": #else without #if.";
				return false;
			}
			Conditional& conditional = conditionals.back();
			conditional.active = conditional.parentActive && !conditional.branchTaken;
			conditional.branchTaken = true;
			continue;
		}
		if (command == "endif") {
			if (conditionals.empty()) {
				error = "line " + std::to_string(lineNumber) + ": #endif without #if.";
				return false;
			}
			conditionals.pop_back();
			continue;
		}
		if (!active())
			continue;
		if (command == "define") {
			std::istringstream definition(argument);
			std::string name;
			definition >> name;
			std::string value;
			std::getline(definition, value);
			const size_t open = name.find('(');
			if (open != std::string::npos && name.back() == ')') {
				FunctionMacro macro;
				std::istringstream arguments(name.substr(open + 1, name.size() - open - 2));
				for (std::string parameter; std::getline(arguments, parameter, ',');)
					macro.arguments.push_back(trim(parameter));
				if (macro.arguments.size() == 1 && macro.arguments[0].empty()) macro.arguments.clear();
				if (std::all_of(macro.arguments.begin(), macro.arguments.end(), [](const std::string& parameter) {
						return !parameter.empty() && isIdentifierStart(parameter.front())
								&& std::all_of(parameter.begin() + 1, parameter.end(), isIdentifierPart);
				})) {
					macro.body = trim(value);
					functionDefines[name.substr(0, open)] = std::move(macro);
					defines[name.substr(0, open)] = "1";
				}
				else
				{
					error = "line " + std::to_string(lineNumber) + ": invalid function-like macro declaration.";
					return false;
				}
			}
			else
				defines[name] = trim(value).empty() ? "1" : trim(value);
		}
		else if (command == "undef") {
			defines.erase(argument);
			functionDefines.erase(argument);
		}
		output += line + '\n';
	}
	if (!conditionals.empty()) {
		error = "unterminated ReShade FX conditional directive.";
		return false;
	}
	return true;
}

bool lex(const std::string& source, std::vector<Token>& tokens, std::string& error)
{
	size_t index = 0;
	size_t line = 1;
	while (index < source.size()) {
		const char current = source[index];
		if (std::isspace(static_cast<unsigned char>(current))) {
			if (current == '\n')
				++line;
			++index;
			continue;
		}
		if (current == '/' && index + 1 < source.size() && source[index + 1] == '/') {
			index += 2;
			while (index < source.size() && source[index] != '\n')
				++index;
			continue;
		}
		if (current == '/' && index + 1 < source.size() && source[index + 1] == '*') {
			const size_t commentLine = line;
			index += 2;
			while (index + 1 < source.size() && !(source[index] == '*' && source[index + 1] == '/')) {
				if (source[index++] == '\n')
					++line;
			}
			if (index + 1 >= source.size()) {
				error = "line " + std::to_string(commentLine) + ": unterminated block comment.";
				return false;
			}
			index += 2;
			continue;
		}
		const size_t begin = index;
		const size_t tokenLine = line;
		if (current == '#') {
			while (index < source.size() && source[index] != '\n')
				++index;
			tokens.push_back({ TokenKind::Directive, source.substr(begin, index - begin), begin, index, tokenLine });
			continue;
		}
		if (isIdentifierStart(current)) {
			++index;
			while (index < source.size() && isIdentifierPart(source[index]))
				++index;
			tokens.push_back({ TokenKind::Identifier, source.substr(begin, index - begin), begin, index, tokenLine });
			continue;
		}
		if (std::isdigit(static_cast<unsigned char>(current)) || (current == '.' && index + 1 < source.size()
				&& std::isdigit(static_cast<unsigned char>(source[index + 1])))) {
			++index;
			while (index < source.size() && (std::isalnum(static_cast<unsigned char>(source[index])) || source[index] == '.'))
				++index;
			tokens.push_back({ TokenKind::Number, source.substr(begin, index - begin), begin, index, tokenLine });
			continue;
		}
		if (current == '"') {
			++index;
			std::string value;
			bool closed = false;
			while (index < source.size()) {
				char valueChar = source[index++];
				if (valueChar == '\\' && index < source.size()) {
					value += source[index++];
					continue;
				}
				if (valueChar == '"') {
					closed = true;
					break;
				}
				if (valueChar == '\n')
					++line;
				value += valueChar;
			}
			if (!closed) {
				error = "line " + std::to_string(tokenLine) + ": unterminated string literal.";
				return false;
			}
			tokens.push_back({ TokenKind::String, std::move(value), begin, index, tokenLine });
			continue;
		}
		tokens.push_back({ TokenKind::Symbol, std::string(1, current), begin, ++index, tokenLine });
	}
	tokens.push_back({ TokenKind::End, {}, source.size(), source.size(), line });
	return true;
}

class Parser {
public:
	Parser(const std::string& source, const std::vector<Token>& tokens, ReShadeEffect& effect, std::string& error)
		: source(source), tokens(tokens), effect(effect), error(error) {}

	bool parse()
	{
		while (!isEnd()) {
			if (current().kind == TokenKind::Directive) {
				if (isIncludeDirective(current().text))
					return fail("includes are not permitted in ReShade FX effects.");
				effect.preprocessorDirectives.push_back(current().text);
				++position;
			}
			else if (match(";")) {
				continue;
			}
			else if (isIdentifier("uniform")) {
				if (!parseUniform())
					return false;
			}
			else if (isIdentifier("technique")) {
				if (!parseTechnique())
					return false;
			}
			else if (isIdentifier("struct")) {
				if (!parseStruct())
					return false;
			}
			else if (!parseTopLevelDeclaration()) {
				return false;
			}
		}
		selectFirstRunnablePass();
		return true;
	}

private:
	const Token& current() const { return tokens[position]; }
	bool isEnd() const { return current().kind == TokenKind::End; }
	bool isIdentifier(const char *value) const { return current().kind == TokenKind::Identifier && current().text == value; }
	bool isSymbol(const char *value) const { return current().kind == TokenKind::Symbol && current().text == value; }
	bool match(const char *value)
	{
		if (!isSymbol(value))
			return false;
		++position;
		return true;
	}
	bool fail(const std::string& message)
	{
		error = "line " + std::to_string(current().line) + ": " + message;
		return false;
	}
	bool takeIdentifier(std::string& value, const char *description)
	{
		if (current().kind != TokenKind::Identifier)
			return fail(std::string("expected ") + description + ".");
		value = current().text;
		++position;
		return true;
	}
	bool consumeBalanced(const char *open, const char *close, size_t& begin, size_t& end)
	{
		if (!match(open))
			return fail(std::string("expected '") + open + "'.");
		begin = tokens[position - 1].begin;
		int depth = 1;
		while (!isEnd()) {
			if (match(open))
				++depth;
			else if (match(close) && --depth == 0) {
				end = tokens[position - 1].end;
				return true;
			}
			else {
				++position;
			}
		}
		return fail(std::string("unterminated '") + open + "' block.");
	}
	std::string range(size_t begin, size_t end) const { return source.substr(begin, end - begin); }
	static bool isAnnotationType(const std::string& value)
	{
		const std::string type = lowercase(value);
		return type == "bool" || type == "int" || type == "uint" || type == "float" || type == "string";
	}
	bool parseAnnotations(std::vector<ReShadeAnnotation>& annotations)
	{
		if (!isSymbol("<"))
			return true;
		++position;
		while (!isEnd() && !isSymbol(">")) {
		std::string name;
			if (!takeIdentifier(name, "annotation name"))
				return false;
			// FX annotations may carry an optional scalar type, for example
			// "string source = \"color\"". The type does not change how the
			// render graph interprets the key, so retain only that key.
			if (isAnnotationType(name) && current().kind == TokenKind::Identifier) {
				std::string typedName;
				if (!takeIdentifier(typedName, "annotation name"))
					return false;
				name = std::move(typedName);
			}
			if (!match("="))
				return fail("expected '=' after annotation name.");
			if (isSymbol(";") || isSymbol(">"))
				return fail("expected annotation value.");
			if (current().kind == TokenKind::String) {
				annotations.push_back({ std::move(name), current().text });
				++position;
				if (isSymbol(";"))
					++position;
				continue;
			}
			const size_t valueBegin = current().begin;
			size_t valueEnd = valueBegin;
			while (!isEnd() && !isSymbol(";") && !isSymbol(">")) {
				valueEnd = current().end;
				++position;
			}
			annotations.push_back({ std::move(name), range(valueBegin, valueEnd) });
			if (isSymbol(";"))
				++position;
		}
		if (!match(">"))
			return fail("unterminated annotation block.");
		return true;
	}
	bool skipArraySuffix()
	{
		while (isSymbol("[")) {
			size_t begin;
			size_t end;
			if (!consumeBalanced("[", "]", begin, end))
				return false;
		}
		return true;
	}
	bool consumeStatement(size_t begin, std::string& definition)
	{
		int braces = 0;
		while (!isEnd()) {
			if (isSymbol("{"))
				++braces;
			else if (isSymbol("}") && braces > 0)
				--braces;
			else if (isSymbol(";") && braces == 0) {
				definition = range(begin, current().end);
				++position;
				return true;
			}
			++position;
		}
		return fail("unterminated declaration.");
	}
	bool parseUniform()
	{
		++position;
		ReShadeUniform uniform;
		if (!takeIdentifier(uniform.typeName, "uniform type") || !takeIdentifier(uniform.name, "uniform name") || !skipArraySuffix()
				|| !parseAnnotations(uniform.annotations))
			return false;
		if (match("=")) {
			if (isSymbol(";"))
				return fail("expected uniform default value.");
			const size_t begin = current().begin;
			size_t end = begin;
			while (!isEnd() && !isSymbol(";")) {
				end = current().end;
				++position;
			}
			uniform.defaultValue = range(begin, end);
		}
		if (!match(";"))
			return fail("expected ';' after uniform declaration.");
		uniform.valueType = valueType(uniform.typeName);
		effect.uniforms.push_back(std::move(uniform));
		return true;
	}
	bool parseStruct()
	{
		++position;
		ReShadeStruct structure;
		if (!takeIdentifier(structure.name, "struct name"))
			return false;
		size_t begin;
		size_t end;
		if (!consumeBalanced("{", "}", begin, end))
			return false;
		structure.body = range(begin + 1, end - 1);
		if (isSymbol(";"))
			++position;
		effect.structs.push_back(std::move(structure));
		return true;
	}
	bool parseTechnique()
	{
		const size_t line = current().line;
		++position;
		ReShadeTechnique technique;
		technique.line = line;
		if (current().kind == TokenKind::Identifier) {
			technique.name = current().text;
			++position;
		}
		std::vector<ReShadeAnnotation> annotations;
		if (!parseAnnotations(annotations))
			return false;
		if (!match("{"))
			return fail("expected technique body.");
		while (!isEnd() && !isSymbol("}")) {
			if (isIdentifier("pass")) {
				ReShadePass pass;
				if (!parsePass(pass))
					return false;
				technique.passes.push_back(std::move(pass));
			}
			else if (!skipUnknownDeclaration()) {
				return false;
			}
		}
		if (!match("}"))
			return fail("unterminated technique.");
		if (isSymbol(";"))
			++position;
		effect.techniques.push_back(std::move(technique));
		return true;
	}
	bool parsePass(ReShadePass& pass)
	{
		pass.line = current().line;
		++position;
		if (current().kind == TokenKind::Identifier) {
			pass.name = current().text;
			++position;
		}
		std::vector<ReShadeAnnotation> annotations;
		if (!parseAnnotations(annotations) || !match("{"))
			return isSymbol("{") ? false : fail("expected pass state block.");
		const size_t bodyBegin = tokens[position - 1].end;
		while (!isEnd() && !isSymbol("}")) {
			if (isIdentifier("VertexShader") || isIdentifier("PixelShader")) {
				const bool pixelShader = isIdentifier("PixelShader");
				++position;
				if (!match("="))
					return fail("expected '=' in pass shader assignment.");
				std::string entryPoint;
				if (!takeIdentifier(entryPoint, "shader entry point") || !match(";"))
					return false;
				if (pixelShader)
					pass.pixelShader = std::move(entryPoint);
				else
					pass.vertexShader = std::move(entryPoint);
			}
			else if (!skipUnknownDeclaration()) {
				return false;
			}
		}
		if (!isSymbol("}"))
			return fail("unterminated pass state block.");
		pass.stateBlock = range(bodyBegin, current().begin);
		++position;
		if (isSymbol(";"))
			++position;
		return true;
	}
	bool parseTopLevelDeclaration()
	{
		if (current().kind != TokenKind::Identifier)
			return fail("expected a top-level declaration.");
		const size_t declarationBegin = current().begin;
		const std::string typeName = current().text;
		++position;
		if (isResourceType(typeName))
			return parseResource(typeName, declarationBegin);
		std::string name;
		if (!takeIdentifier(name, "declaration name"))
			return false;
		if (isSymbol("("))
			return parseFunction(typeName, name, declarationBegin);
		std::string ignored;
		return consumeStatement(declarationBegin, ignored);
	}
	bool parseResource(std::string typeName, size_t declarationBegin)
	{
		if (isSymbol("<")) {
			size_t ignoredBegin;
			size_t typeEnd;
			if (!consumeBalanced("<", ">", ignoredBegin, typeEnd))
				return false;
			typeName = range(declarationBegin, typeEnd);
		}
		std::string name;
		if (!takeIdentifier(name, "resource name") || !skipArraySuffix())
			return false;
		std::vector<ReShadeAnnotation> annotations;
		if (!parseAnnotations(annotations))
			return false;
		std::string definition;
		if (!consumeStatement(declarationBegin, definition))
			return false;
		if (isTextureType(typeName))
			effect.textures.push_back({ std::move(typeName), std::move(name), std::move(definition), std::move(annotations) });
		else
			effect.samplers.push_back({ std::move(typeName), std::move(name), std::move(definition), std::move(annotations) });
		return true;
	}
	bool parseFunction(const std::string& returnType, const std::string& name, size_t declarationBegin)
	{
		const size_t declarationLine = current().line;
		size_t ignoredBegin;
		size_t ignoredEnd;
		if (!consumeBalanced("(", ")", ignoredBegin, ignoredEnd))
			return false;
		while (!isEnd() && !isSymbol("{") && !isSymbol(";"))
			++position;
		if (match(";"))
			return true;
		if (!isSymbol("{"))
			return fail("expected function body.");
		const size_t bodyOpen = current().begin;
		size_t ignoredBodyBegin;
		size_t bodyEnd;
		if (!consumeBalanced("{", "}", ignoredBodyBegin, bodyEnd))
			return false;
		effect.functions.push_back({ returnType, name, range(declarationBegin, bodyOpen), range(bodyOpen + 1, bodyEnd - 1),
				declarationLine });
		return true;
	}
	bool skipUnknownDeclaration()
	{
		if (isSymbol("{")) {
			size_t begin;
			size_t end;
			return consumeBalanced("{", "}", begin, end);
		}
		while (!isEnd() && !isSymbol(";") && !isSymbol("}"))
			++position;
		if (match(";"))
			return true;
		return !isEnd();
	}
	void selectFirstRunnablePass()
	{
		for (size_t techniqueIndex = 0; techniqueIndex < effect.techniques.size(); ++techniqueIndex) {
			for (size_t passIndex = 0; passIndex < effect.techniques[techniqueIndex].passes.size(); ++passIndex) {
				if (!effect.techniques[techniqueIndex].passes[passIndex].pixelShader.empty()) {
					effect.selectedTechnique = techniqueIndex;
					effect.selectedPass = passIndex;
					return;
				}
			}
		}
	}
	static ReShadeValueType valueType(const std::string& typeName)
	{
		const std::string type = lowercase(typeName);
		if (type == "bool") return ReShadeValueType::Bool;
		if (type == "int") return ReShadeValueType::Int;
		if (type == "uint") return ReShadeValueType::UInt;
		if (type == "float") return ReShadeValueType::Float;
		if (type == "float2") return ReShadeValueType::Float2;
		if (type == "float3") return ReShadeValueType::Float3;
		if (type == "float4") return ReShadeValueType::Float4;
		if (type == "float2x2") return ReShadeValueType::Float2x2;
		if (type == "float3x3") return ReShadeValueType::Float3x3;
		if (type == "float4x4") return ReShadeValueType::Float4x4;
		return ReShadeValueType::Unknown;
	}
	static bool isTextureType(const std::string& typeName)
	{
		return lowercase(typeName).find("texture") != std::string::npos;
	}
	static bool isResourceType(const std::string& typeName)
	{
		const std::string type = lowercase(typeName);
		return type.find("texture") != std::string::npos || type.find("sampler") != std::string::npos;
	}

	const std::string& source;
	const std::vector<Token>& tokens;
	ReShadeEffect& effect;
	std::string& error;
	size_t position = 0;
};

} // namespace

bool parseReShadeEffect(const std::string& source, ReShadeEffect& effect, std::string& error)
{
	std::string preprocessed;
	if (!preprocess(source, preprocessed, error))
		return false;
	std::vector<Token> tokens;
	if (!lex(preprocessed, tokens, error))
		return false;
	ReShadeEffect parsed;
	Parser parser(preprocessed, tokens, parsed, error);
	if (!parser.parse())
		return false;
	effect = std::move(parsed);
	error.clear();
	return true;
}

} // namespace postprocess
