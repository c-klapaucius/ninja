// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NINJA_MANIFEST_PARSER_H_
#define NINJA_MANIFEST_PARSER_H_

#include <string>

using namespace std;

#include "lexer.h"

struct BindingEnv;
struct EvalString;
struct FileReader;
struct State;

enum DupeEdgeAction {
  kDupeEdgeActionWarn,
  kDupeEdgeActionError,
};

/// Parses .ninja files.
struct ManifestParser {
  ManifestParser(State* state, FileReader* file_reader,
                 DupeEdgeAction dupe_edge_action);

  /// Load and parse a file.
  bool Load(const string& filename, string* err, Lexer* parent = NULL);

  /// Parse a text string of input.  Used by tests.
  bool ParseTest(const string& input, string* err) {
    quiet_ = true;
    return Parse("input", input, err);
  }

private:
  /// Read a path (complete with $escapes).
  /// Returns false only on error, returned path may be empty if a delimiter
  /// (space, newline) is hit.
  bool ReadPath(EvalString* path, string* err) {
    return ReadEvalString(path, true, err);
  }
  /// Read the value side of a var = value line (complete with $escapes).
  /// Returns false only on error.
  bool ReadVarValue(EvalString* value, string* err) {
    return ReadEvalString(value, false, err);
  }
  bool ReadEvalString(EvalString* eval, bool path, string* err);
  /// Parse a file, given its contents as a string.
  bool Parse(const string& filename, const string& input, string* err);

  /// Parse various statement types.
  bool ParsePool(string* err);
  bool ParseRule(string* err);
  bool ParseLet(string* key, EvalString* val, string* err);
  bool ParseLet(string* key, EvalString* val, bool &pluseq, string* err);
  bool ParseEdge(string* err);
  bool ParseDefault(string* err);
  bool ParseFor(string* err);
  bool ParseEnd(string* err);

  /// Check and produce error for 'for' without 'end for'
  bool CheckForEndExpected(string* err);

  /// Parse either a 'subninja' or 'include' line.
  bool ParseFileInclude(bool new_scope, string* err);

  /// If the next token is not \a expected, produce an error string
  /// saying "expectd foo, got bar".
  bool ExpectToken(Lexer::Token expected, string* err);

  State* state_;
  BindingEnv* env_;
  FileReader* file_reader_;
  Lexer lexer_;
  DupeEdgeAction dupe_edge_action_;
  bool quiet_;
  string subinput_;
  Lexer sublexer_;
};

#endif  // NINJA_MANIFEST_PARSER_H_
