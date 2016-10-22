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

#include "manifest_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "disk_interface.h"
#include "graph.h"
#include "metrics.h"
#include "state.h"
#include "util.h"
#include "version.h"

ManifestParser::ManifestParser(State* state, FileReader* file_reader,
                               DupeEdgeAction dupe_edge_action)
    : state_(state), file_reader_(file_reader),
      dupe_edge_action_(dupe_edge_action), quiet_(false) {
  env_ = &state->bindings_;
}

bool ManifestParser::Load(const string& filename, string* err, Lexer* parent) {
  METRIC_RECORD(".ninja parse");
  string contents;
  string read_err;
  if (file_reader_->ReadFile(filename, &contents, &read_err) != FileReader::Okay) {
    *err = "loading '" + filename + "': " + read_err;
    if (parent)
      parent->Error(string(*err), err);
    return false;
  }

  // The lexer needs a nul byte at the end of its input, to know when it's done.
  // It takes a StringPiece, and StringPiece's string constructor uses
  // string::data().  data()'s return value isn't guaranteed to be
  // null-terminated (although in practice - libc++, libstdc++, msvc's stl --
  // it is, and C++11 demands that too), so add an explicit nul byte.
  contents.resize(contents.size() + 1);

  return Parse(filename, contents, err);
}

bool ManifestParser::Parse(const string& filename, const string& input,
                           string* err) {
  lexer_.Start(filename, input);

  for (;;) {
    Lexer::Token token = lexer_.ReadToken();
    switch (token) {
    case Lexer::POOL:
      if (!ParsePool(err))
        return false;
      break;
    case Lexer::BUILD:
      if (!ParseEdge(err))
        return false;
      break;
    case Lexer::RULE:
      if (!ParseRule(err))
        return false;
      break;
    case Lexer::DEFAULT:
      if (!ParseDefault(err))
        return false;
      break;
    case Lexer::IDENT: {
      lexer_.UnreadToken();
      string name;
      EvalString let_value;
      bool pluseq=false;
      if (!ParseLet(&name, &let_value, pluseq, err))
        return false;
      string value = let_value.Evaluate(env_);
      // Check ninja_required_version immediately so we can exit
      // before encountering any syntactic surprises.
      if (name == "ninja_required_version")
        CheckNinjaVersion(value);
      if (pluseq) {
        env_->AddBinding(name, env_->LookupVariable(name) + value);
      } else {
        env_->AddBinding(name, value);
      }
      break;
    }
    case Lexer::INCLUDE:
      if (!ParseFileInclude(false, err))
        return false;
      break;
    case Lexer::SUBNINJA:
      if (!ParseFileInclude(true, err))
        return false;
      break;
    case Lexer::FOR:
      if (!ParseFor(err))
        return false;
      break;
    case Lexer::END:
      if (!ParseEnd(err))
        return false;
    break;
    case Lexer::ERROR: {
      return lexer_.Error(lexer_.DescribeLastError(), err);
    }
    case Lexer::TEOF:
      if (!CheckForEndExpected(err))
        return false;
      return true;
    case Lexer::NEWLINE:
      break;
    default:
      return lexer_.Error(string("unexpected ") + Lexer::TokenName(token),
                          err);
    }
  }
  return false;  // not reached
}


bool ManifestParser::ParsePool(string* err) {
  string name;
  if (!lexer_.ReadIdent(&name))
    return lexer_.Error("expected pool name", err);

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  if (state_->LookupPool(name) != NULL)
    return lexer_.Error("duplicate pool '" + name + "'", err);

  int depth = -1;

  while (lexer_.PeekToken(Lexer::INDENT)) {
    string key;
    EvalString value;
    if (!ParseLet(&key, &value, err))
      return false;

    if (key == "depth") {
      string depth_string = value.Evaluate(env_);
      depth = atol(depth_string.c_str());
      if (depth < 0)
        return lexer_.Error("invalid pool depth", err);
    } else {
      return lexer_.Error("unexpected variable '" + key + "'", err);
    }
  }

  if (depth < 0)
    return lexer_.Error("expected 'depth =' line", err);

  state_->AddPool(new Pool(name, depth));
  return true;
}

bool ManifestParser::ParseFor(string* err) {
  ForLoop loop;
  if (!lexer_.ReadIdent(&loop.key))
    return lexer_.Error("expected variable name", err);
  if (!ExpectToken(Lexer::IN_, err))
    return false;
  EvalString out;
  while (1) {
    out.Clear();
    if (!ReadPath(&out, err))
      return false;
    if (out.empty()) {
      if (loop.values.size()==0)
        return lexer_.Error("expected path", err);
      break;
    }
    loop.values.push_back(out);
  }

  loop.i=0;
  if (loop.values.size()) {
    string value=loop.values[loop.i++].Evaluate(env_);
    env_->AddBinding(loop.key, value);
    lexer_.StoreTokenPos(loop.lexpos);
  }
  state_->forloops_.push_back(loop);
  return true;
}

bool ManifestParser::ParseEnd(string* err) {
  // syntactic sugar: expect 'end for', not just 'end'
  if (!ExpectToken(Lexer::FOR, err))
    return false;
  if (state_->forloops_.size()<1)
    return lexer_.Error("'end for' without 'for'", err);
  ForLoop &loop=state_->forloops_.back();
  if (loop.i>=loop.values.size()) {
    state_->forloops_.pop_back();
  } else {
	  string value=loop.values[loop.i++].Evaluate(env_);
    env_->AddBinding(loop.key, value);
    // Somewhat rude: unread all tokens and go to the line after for statement
    lexer_.RestoreTokenPos(loop.lexpos);
  }
  return true;
}

bool ManifestParser::CheckForEndExpected(string* err) {
  if (state_->forloops_.size())
    return lexer_.Error("'end for' expected", err);
  return true;
}

bool ManifestParser::ParseRule(string* err) {
  string name;
  if (!lexer_.ReadIdent(&name))
    return lexer_.Error("expected rule name", err);

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  if (env_->LookupRuleCurrentScope(name) != NULL)
    return lexer_.Error("duplicate rule '" + name + "'", err);

  Rule* rule = new Rule(name);  // XXX scoped_ptr

  while (lexer_.PeekToken(Lexer::INDENT)) {
    string key;
    EvalString value;
    if (!ParseLet(&key, &value, err))
      return false;

    if (Rule::IsReservedBinding(key)) {
      rule->AddBinding(key, value);
    } else {
      // Die on other keyvals for now; revisit if we want to add a
      // scope here.
      return lexer_.Error("unexpected variable '" + key + "'", err);
    }
  }

  if (rule->bindings_["rspfile"].empty() !=
      rule->bindings_["rspfile_content"].empty()) {
    return lexer_.Error("rspfile and rspfile_content need to be "
                        "both specified", err);
  }

  if (rule->bindings_["command"].empty())
    return lexer_.Error("expected 'command =' line", err);

  env_->AddRule(rule);
  return true;
}

bool ManifestParser::ReadEvalString(EvalString* eval, bool path, string* err) {
  if (subinput_.length()) {
    if (!sublexer_.ReadEvalString(eval, NULL, path, err)) {
      subinput_="";
      string errmsg="Error expanding $( variable ) with messsage '";
      errmsg+=err ? *err : "NULL";
      errmsg+"'";
      return lexer_.Error(errmsg, err);
    }
    if (!eval->empty())
      return true;
    subinput_="";
  }
  string special2;
  if (!lexer_.ReadEvalString(eval, &special2, path, err))
    return false;
  if (special2.length()>0) {
    subinput_=env_->LookupVariable(special2) + "\n";
    string filename=string("parsing *$(") + special2 +")";
    sublexer_.Start(filename, subinput_);
    sublexer_.EatWhitespace();
    if (eval->empty())
      return ReadEvalString(eval, path, err);
  }
  return true;
}

bool ManifestParser::ParseLet(string* key, EvalString* value, string* err) {
  if (!lexer_.ReadIdent(key))
    return lexer_.Error("expected variable name", err);
  if (!ExpectToken(Lexer::EQUALS, err))
    return false;
  if (!ReadVarValue(value, err))
    return false;
  return true;
}

bool ManifestParser::ParseLet(string* key, EvalString* value, bool &pluseq, string* err) {
  if (!lexer_.ReadIdent(key))
    return lexer_.Error("expected variable name", err);

  Lexer::Token token = lexer_.ReadToken();
  pluseq=(token==Lexer::PLUSEQ);
  if (token!=Lexer::EQUALS && token!=Lexer::PLUSEQ) {
    string message = string("expected ") + Lexer::TokenName(Lexer::EQUALS) + " or " + Lexer::TokenName(Lexer::PLUSEQ);
    message += string(", got ") + Lexer::TokenName(token);
    message += Lexer::TokenErrorHint(Lexer::EQUALS);
    message += Lexer::TokenErrorHint(Lexer::PLUSEQ);
    return lexer_.Error(message, err);
  }
  if (!ReadVarValue(value, err))
    return false;
  return true;
}

bool ManifestParser::ParseDefault(string* err) {
  EvalString eval;
  if (!ReadPath(&eval, err))
    return false;
  if (eval.empty())
    return lexer_.Error("expected target name", err);

  do {
    string path = eval.Evaluate(env_);
    string path_err;
    unsigned int slash_bits;  // Unused because this only does lookup.
    if (!CanonicalizePath(&path, &slash_bits, &path_err))
      return lexer_.Error(path_err, err);
    if (!state_->AddDefault(path, &path_err))
      return lexer_.Error(path_err, err);

    eval.Clear();
    if (!ReadPath(&eval, err))
      return false;
  } while (!eval.empty());

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  return true;
}

bool ManifestParser::ParseEdge(string* err) {
  vector<EvalString> ins, outs;

  {
    EvalString out;
    if (!ReadPath(&out, err))
      return false;
    if (out.empty())
      return lexer_.Error("expected path", err);

    do {
      outs.push_back(out);

      out.Clear();
      if (!ReadPath(&out, err))
        return false;
    } while (!out.empty());
  }

  // Add all implicit outs, counting how many as we go.
  int implicit_outs = 0;
  if (lexer_.PeekToken(Lexer::PIPE)) {
    for (;;) {
      EvalString out;
      if (!ReadPath(&out, err))
        return err;
      if (out.empty())
        break;
      outs.push_back(out);
      ++implicit_outs;
    }
  }

  if (!ExpectToken(Lexer::COLON, err))
    return false;

  string rule_name;
  if (!lexer_.ReadIdent(&rule_name))
    return lexer_.Error("expected build command name", err);

  const Rule* rule = env_->LookupRule(rule_name);
  if (!rule)
    return lexer_.Error("unknown build rule '" + rule_name + "'", err);

  for (;;) {
    // XXX should we require one path here?
    EvalString in;
    if (!ReadPath(&in, err))
      return false;
    if (in.empty())
      break;
    ins.push_back(in);
  }

  // Add all implicit deps, counting how many as we go.
  int implicit = 0;
  if (lexer_.PeekToken(Lexer::PIPE)) {
    for (;;) {
      EvalString in;
      if (!ReadPath(&in, err))
        return err;
      if (in.empty())
        break;
      ins.push_back(in);
      ++implicit;
    }
  }

  // Add all order-only deps, counting how many as we go.
  int order_only = 0;
  if (lexer_.PeekToken(Lexer::PIPE2)) {
    for (;;) {
      EvalString in;
      if (!ReadPath(&in, err))
        return false;
      if (in.empty())
        break;
      ins.push_back(in);
      ++order_only;
    }
  }

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  // Bindings on edges are rare, so allocate per-edge envs only when needed.
  bool has_indent_token = lexer_.PeekToken(Lexer::INDENT);
  BindingEnv* env = has_indent_token ? new BindingEnv(env_) : env_;
  while (has_indent_token) {
    string key;
    EvalString val;
    bool pluseq=false;
    if (!ParseLet(&key, &val, pluseq, err))
      return false;
    if (pluseq) {
      env->AddBinding(key, env->LookupVariable(key) + val.Evaluate(env_));
    } else {
      env->AddBinding(key, val.Evaluate(env_));
    }
    has_indent_token = lexer_.PeekToken(Lexer::INDENT);
  }

  Edge* edge = state_->AddEdge(rule);
  edge->env_ = env;

  string pool_name = edge->GetBinding("pool");
  if (!pool_name.empty()) {
    Pool* pool = state_->LookupPool(pool_name);
    if (pool == NULL)
      return lexer_.Error("unknown pool name '" + pool_name + "'", err);
    edge->pool_ = pool;
  }

  edge->outputs_.reserve(outs.size());
  for (size_t i = 0, e = outs.size(); i != e; ++i) {
    string path = outs[i].Evaluate(env);
    string path_err;
    unsigned int slash_bits;
    if (!CanonicalizePath(&path, &slash_bits, &path_err))
      return lexer_.Error(path_err, err);
    if (!state_->AddOut(edge, path, slash_bits)) {
      if (dupe_edge_action_ == kDupeEdgeActionError) {
        lexer_.Error("multiple rules generate " + path + " [-w dupbuild=err]",
                     err);
        return false;
      } else {
        if (!quiet_) {
          Warning("multiple rules generate %s. "
                  "builds involving this target will not be correct; "
                  "continuing anyway [-w dupbuild=warn]",
                  path.c_str());
        }
        if (e - i <= static_cast<size_t>(implicit_outs))
          --implicit_outs;
      }
    }
  }
  if (edge->outputs_.empty()) {
    // All outputs of the edge are already created by other edges. Don't add
    // this edge.  Do this check before input nodes are connected to the edge.
    state_->edges_.pop_back();
    delete edge;
    return true;
  }
  edge->implicit_outs_ = implicit_outs;

  edge->inputs_.reserve(ins.size());
  for (vector<EvalString>::iterator i = ins.begin(); i != ins.end(); ++i) {
    string path = i->Evaluate(env);
    string path_err;
    unsigned int slash_bits;
    if (!CanonicalizePath(&path, &slash_bits, &path_err))
      return lexer_.Error(path_err, err);
    state_->AddIn(edge, path, slash_bits);
  }
  edge->implicit_deps_ = implicit;
  edge->order_only_deps_ = order_only;

  // Multiple outputs aren't (yet?) supported with depslog.
  string deps_type = edge->GetBinding("deps");
  if (!deps_type.empty() && edge->outputs_.size() > 1) {
    return lexer_.Error("multiple outputs aren't (yet?) supported by depslog; "
                        "bring this up on the mailing list if it affects you",
                        err);
  }

  return true;
}

bool ManifestParser::ParseFileInclude(bool new_scope, string* err) {
  EvalString eval;
  if (!ReadPath(&eval, err))
    return false;
  string path = eval.Evaluate(env_);

  ManifestParser subparser(state_, file_reader_, dupe_edge_action_);
  if (new_scope) {
    subparser.env_ = new BindingEnv(env_);
  } else {
    subparser.env_ = env_;
  }

  if (!subparser.Load(path, err, &lexer_))
    return false;

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  return true;
}

bool ManifestParser::ExpectToken(Lexer::Token expected, string* err) {
  Lexer::Token token = lexer_.ReadToken();
  if (token != expected) {
    string message = string("expected ") + Lexer::TokenName(expected);
    message += string(", got ") + Lexer::TokenName(token);
    message += Lexer::TokenErrorHint(expected);
    return lexer_.Error(message, err);
  }
  return true;
}
