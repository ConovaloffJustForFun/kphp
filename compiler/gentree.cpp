#include "compiler/gentree.h"

#include <sstream>

#include "common/algorithms/find.h"

#include "compiler/compiler-core.h"
#include "compiler/data/class-data.h"
#include "compiler/data/define-data.h"
#include "compiler/data/function-data.h"
#include "compiler/data/src-file.h"
#include "compiler/debug.h"
#include "compiler/io.h"
#include "compiler/name-gen.h"
#include "compiler/phpdoc.h"
#include "compiler/stage.h"
#include "compiler/vertex.h"

#define CE(x) if (!(x)) {return VertexPtr();}

GenTree::GenTree(const vector<Token *> *tokens, SrcFilePtr file, DataStream<FunctionPtr> &os) :
  line_num(0),
  tokens(tokens),
  parsed_os(os),
  in_func_cnt_(0),
  is_top_of_the_function_(false),
  cur(tokens->begin()),
  end(tokens->end()),
  processing_file(file) {                  // = stage::get_file()

  kphp_assert (cur != end);
  end--;
  kphp_assert ((*end)->type() == tok_end);

  line_num = (*cur)->line_num;
  stage::set_line(line_num);
}

inline bool GenTree::in_namespace() const {
  return !processing_file->namespace_name.empty();
}

VertexPtr GenTree::generate_constant_field_class(VertexPtr root) {
  auto name_of_const_field_class = VertexAdaptor<op_string>::create();
  name_of_const_field_class->str_val = "c#" + replace_backslashes(cur_class->name) + "$$class";

  auto value_of_const_field_class = VertexAdaptor<op_string>::create();
  value_of_const_field_class->set_string(cur_class->name);

  auto def = VertexAdaptor<op_define>::create(name_of_const_field_class, value_of_const_field_class);
  def->location = root->location;

  return def;
}

void GenTree::enter_function() {
  in_func_cnt_++;
}

void GenTree::exit_function() {
  in_func_cnt_--;
}

void GenTree::next_cur() {
  if (cur != end) {
    cur++;
    if ((*cur)->line_num != -1) {
      line_num = (*cur)->line_num;
      stage::set_line(line_num);
    }
  }
}

bool GenTree::test_expect(TokenType tp) {
  return (*cur)->type() == tp;
}

#define expect(tp, msg) ({ \
  bool res__;\
  if (kphp_error (test_expect (tp), format ("Expected %s, found '%s'", msg, cur == end ? "END OF FILE" : (*cur)->to_str().c_str()))) {\
    res__ = false;\
  } else {\
    next_cur();\
    res__ = true;\
  }\
  res__; \
})

#define expect2(tp1, tp2, msg) ({ \
  kphp_error (test_expect (tp1) || test_expect (tp2), format ("Expected %s, found '%s'", msg, cur == end ? "END OF FILE" : (*cur)->to_str().c_str())); \
  if (cur != end) {next_cur();} \
  1;\
})

VertexPtr GenTree::get_var_name() {
  AutoLocation var_location(this);

  if ((*cur)->type() != tok_var_name) {
    return VertexPtr();
  }
  auto var = VertexAdaptor<op_var>::create();
  var->str_val = static_cast<string>((*cur)->str_val);

  set_location(var, var_location);

  next_cur();
  return var;
}

VertexPtr GenTree::get_var_name_ref() {
  int ref_flag = 0;
  if ((*cur)->type() == tok_and) {
    next_cur();
    ref_flag = 1;
  }

  VertexPtr name = get_var_name();
  if (name) {
    name->ref_flag = ref_flag;
  } else {
    kphp_error (ref_flag == 0, "Expected var name");
  }
  return name;
}

int GenTree::open_parent() {
  if ((*cur)->type() == tok_oppar) {
    next_cur();
    return 1;
  }
  return 0;
}

inline void GenTree::skip_phpdoc_tokens() {
  while ((*cur)->type() == tok_phpdoc) {
    next_cur();
  }
}

#define close_parent(is_opened)\
  if (is_opened) {\
    CE (expect (tok_clpar, "')'"));\
  }

template<Operation EmptyOp>
bool GenTree::gen_list(vector<VertexPtr> *res, GetFunc f, TokenType delim) {
  //Do not clear res. Result must be appended to it.
  bool prev_delim = false;
  bool next_delim = true;

  while (next_delim) {
    VertexPtr v = (this->*f)();
    next_delim = (*cur)->type() == delim;

    if (!v) {
      if (EmptyOp != op_err && (prev_delim || next_delim)) {
        if (EmptyOp == op_none) {
          break;
        }
        auto tmp = VertexAdaptor<EmptyOp>::create();
        v = tmp;
      } else if (prev_delim) {
        kphp_error (0, "Expected something after ','");
        return false;
      } else {
        break;
      }
    }

    res->push_back(v);
    prev_delim = true;

    if (next_delim) {
      next_cur();
    }
  }

  return true;
}

template<Operation Op>
VertexPtr GenTree::get_conv() {
  AutoLocation conv_location(this);
  next_cur();
  VertexPtr first_node = get_expression();
  CE (!kphp_error(first_node, "get_conv failed"));
  auto conv = VertexAdaptor<Op>::create(first_node);
  set_location(conv, conv_location);
  return conv;
}

template<Operation Op>
VertexPtr GenTree::get_varg_call() {
  AutoLocation call_location(this);
  next_cur();

  CE (expect(tok_oppar, "'('"));

  AutoLocation args_location(this);
  vector<VertexPtr> args_next;
  bool ok_args_next = gen_list<op_err>(&args_next, &GenTree::get_expression, tok_comma);
  CE (!kphp_error(ok_args_next, "get_varg_call failed"));
  auto args = VertexAdaptor<op_array>::create(args_next);
  set_location(args, args_location);

  CE (expect(tok_clpar, "')'"));

  auto call = VertexAdaptor<Op>::create(args);
  set_location(call, call_location);
  return call;
}

VertexPtr GenTree::get_require() {
  AutoLocation require_location(this);
  next_cur();
  const bool is_opened = open_parent();
  auto require = VertexAdaptor<op_require>::create(GenTree::get_expression());
  close_parent (is_opened);
  set_location(require, require_location);
  return require;
}

VertexPtr GenTree::get_require_once() {
  AutoLocation require_location(this);
  next_cur();
  bool is_opened = open_parent();
  vector<VertexPtr> requires;
  bool ok_next = gen_list<op_err>(&requires, &GenTree::get_expression, tok_comma);
  CE (!kphp_error(ok_next, "get_require_list for require_once failed"));
  close_parent (is_opened);
  VertexPtr require_once_seq;
  if (requires.size() == 1) {
    require_once_seq = VertexAdaptor<op_require_once>::create(requires.back());
  } else {
    for (auto &require: requires) {
      require = VertexAdaptor<op_require_once>::create(require);
    }
    require_once_seq = VertexAdaptor<op_seq>::create(std::move(requires));
  }
  set_location(require_once_seq, require_location);
  return require_once_seq;
}

template<Operation Op, Operation EmptyOp>
VertexPtr GenTree::get_func_call() {
  AutoLocation call_location(this);
  string name{(*cur)->str_val};
  next_cur();

  CE (expect(tok_oppar, "'('"));
  skip_phpdoc_tokens();
  vector<VertexPtr> next;
  bool ok_next = gen_list<EmptyOp>(&next, &GenTree::get_expression, tok_comma);
  CE (!kphp_error(ok_next, "get argument list failed"));
  CE (expect(tok_clpar, "')'"));

  if (Op == op_isset) {
    CE (!kphp_error(!next.empty(), "isset function requires at least one argument"));
    VertexPtr left = VertexAdaptor<op_isset>::create(next[0]);
    for (size_t i = 1; i < next.size(); i++) {
      auto right = VertexAdaptor<op_isset>::create(next[i]);
      auto log_and = VertexAdaptor<op_log_and>::create(left, right);
      left = log_and;
    }
    set_location(left, call_location);
    return left;
  }

  auto call = VertexAdaptor<Op>::create(next);
  set_location(call, call_location);

  //hack..
  if (Op == op_func_call) {
    VertexAdaptor<op_func_call> func_call = call;
    func_call->set_string(name);
  }
  if (Op == op_constructor_call) {
    VertexAdaptor<op_constructor_call> func_call = call;
    func_call->set_string(name);

    // todo optimize
    if (name.size() == 8 && name == "Memcache") {
      func_call->type_help = tp_MC;
    }
    if (name == "true_mc" || name == "test_mc" || name == "RpcMemcache") {
      func_call->type_help = tp_MC;
    }
    if (name.size() == 9 && name == "Exception") {
      func_call->type_help = tp_Exception;
    }
    if (name.size() == 10 && name == "\\Exception") {
      func_call->set_string("Exception");
      func_call->type_help = tp_Exception;
    }
  }
  return call;
}

VertexPtr GenTree::get_short_array() {
  AutoLocation call_location(this);
  next_cur();

  vector<VertexPtr> next;
  bool ok_next = gen_list<op_none>(&next, &GenTree::get_expression, tok_comma);
  CE (!kphp_error(ok_next, "get short array failed"));
  CE (expect(tok_clbrk, "']'"));

  auto arr = VertexAdaptor<op_array>::create(next);
  set_location(arr, call_location);

  return arr;
}


VertexPtr GenTree::get_string() {
  auto str = VertexAdaptor<op_string>::create();
  set_location(str, AutoLocation(this));
  str->str_val = static_cast<string>((*cur)->str_val);
  next_cur();
  return str;
}

VertexPtr GenTree::get_string_build() {
  AutoLocation sb_location(this);
  vector<VertexPtr> v_next;
  next_cur();
  bool after_simple_expression = false;
  while (cur != end && (*cur)->type() != tok_str_end) {
    if ((*cur)->type() == tok_str) {
      v_next.push_back(get_string());
      if (after_simple_expression) {
        VertexAdaptor<op_string> last = v_next.back().as<op_string>();
        if (last->str_val != "" && last->str_val[0] == '[') {
          kphp_warning("Simple string expressions with [] can work wrong. Use more {}");
        }
      }
      after_simple_expression = false;
    } else if ((*cur)->type() == tok_expr_begin) {
      next_cur();

      VertexPtr add = get_expression();
      CE (!kphp_error(add, "Bad expression in string"));
      v_next.push_back(add);

      CE (expect(tok_expr_end, "'}'"));
      after_simple_expression = false;
    } else {
      after_simple_expression = true;
      VertexPtr add = get_expression();
      CE (!kphp_error(add, "Bad expression in string"));
      v_next.push_back(add);
    }
  }
  CE (expect(tok_str_end, "'\"'"));
  auto sb = VertexAdaptor<op_string_build>::create(v_next);
  set_location(sb, sb_location);
  return sb;
}

VertexPtr GenTree::get_postfix_expression(VertexPtr res) {
  //postfix operators ++, --, [], ->
  bool need = true;
  while (need && cur != end) {
    vector<Token *>::const_iterator op = cur;
    TokenType tp = (*op)->type();
    need = false;

    if (tp == tok_inc) {
      auto v = VertexAdaptor<op_postfix_inc>::create(res);
      set_location(v, AutoLocation(this));
      res = v;
      need = true;
      next_cur();
    } else if (tp == tok_dec) {
      auto v = VertexAdaptor<op_postfix_dec>::create(res);
      set_location(v, AutoLocation(this));
      res = v;
      need = true;
      next_cur();
    } else if (tp == tok_opbrk || tp == tok_opbrc) {
      AutoLocation location(this);
      next_cur();
      VertexPtr i = get_expression();
      if (tp == tok_opbrk) {
        CE (expect(tok_clbrk, "']'"));
      } else {
        CE (expect(tok_clbrc, "'}'"));
      }
      //TODO: it should be to separate operations
      if (!i) {
        auto v = VertexAdaptor<op_index>::create(res);
        res = v;
      } else {
        auto v = VertexAdaptor<op_index>::create(res, i);
        res = v;
      }
      set_location(res, location);
      need = true;
    } else if (tp == tok_arrow) {
      AutoLocation location(this);
      next_cur();
      VertexPtr i = get_expr_top();
      CE (!kphp_error(i, "Failed to parse right argument of '->'"));
      auto v = VertexAdaptor<op_arrow>::create(res, i);
      res = v;
      set_location(res, location);
      need = true;
    } else if (tp == tok_oppar) {
      AutoLocation location(this);
      next_cur();
      skip_phpdoc_tokens();
      vector<VertexPtr> next;
      bool ok_next = gen_list<op_err>(&next, &GenTree::get_expression, tok_comma);
      CE (!kphp_error(ok_next, "get argument list failed"));
      CE (expect(tok_clpar, "')'"));

      auto call = VertexAdaptor<op_func_call>::create(next);
      set_location(call, location);

      call->set_string("__invoke");

      res = VertexAdaptor<op_arrow>::create(res, call);
      set_location(res, location);
      need = true;
    }


  }
  return res;
}

VertexPtr GenTree::get_expr_top() {
  vector<Token *>::const_iterator op = cur;

  VertexPtr res, first_node;
  TokenType type = (*op)->type();

  bool return_flag = true;
  switch (type) {
    case tok_line_c: {
      auto v = VertexAdaptor<op_int_const>::create();
      set_location(v, AutoLocation(this));
      v->str_val = int_to_str(stage::get_line());
      res = v;
      next_cur();
      break;
    }
    case tok_file_c: {
      auto v = VertexAdaptor<op_string>::create();
      set_location(v, AutoLocation(this));
      v->str_val = processing_file->file_name;
      next_cur();
      res = v;
      break;
    }
    case tok_func_c: {
      auto v = VertexAdaptor<op_function_c>::create();
      set_location(v, AutoLocation(this));
      next_cur();
      res = v;
      break;
    }
    case tok_int_const: {
      auto v = VertexAdaptor<op_int_const>::create();
      set_location(v, AutoLocation(this));
      v->str_val = static_cast<string>((*cur)->str_val);
      next_cur();
      res = v;
      break;
    }
    case tok_float_const: {
      auto v = VertexAdaptor<op_float_const>::create();
      set_location(v, AutoLocation(this));
      v->str_val = static_cast<string>((*cur)->str_val);
      next_cur();
      res = v;
      break;
    }
    case tok_null: {
      auto v = VertexAdaptor<op_null>::create();
      set_location(v, AutoLocation(this));
      next_cur();
      res = v;
      break;
    }
    case tok_false: {
      auto v = VertexAdaptor<op_false>::create();
      set_location(v, AutoLocation(this));
      next_cur();
      res = v;
      break;
    }
    case tok_true: {
      auto v = VertexAdaptor<op_true>::create();
      set_location(v, AutoLocation(this));
      next_cur();
      res = v;
      break;
    }
    case tok_var_name: {
      res = get_var_name();
      return_flag = false;
      break;
    }
    case tok_str:
      res = get_string();
      break;
    case tok_conv_int:
      res = get_conv<op_conv_int>();
      break;
    case tok_conv_bool:
      res = get_conv<op_conv_bool>();
      break;
    case tok_conv_float:
      res = get_conv<op_conv_float>();
      break;
    case tok_conv_string:
      res = get_conv<op_conv_string>();
      break;
    case tok_conv_long:
      res = get_conv<op_conv_long>();
      break;
    case tok_conv_uint:
      res = get_conv<op_conv_uint>();
      break;
    case tok_conv_ulong:
      res = get_conv<op_conv_ulong>();
      break;
    case tok_conv_array:
      res = get_conv<op_conv_array>();
      break;

    case tok_print: {
      AutoLocation print_location(this);
      next_cur();
      first_node = get_expression();
      CE (!kphp_error(first_node, "Failed to get print argument"));
      first_node = conv_to<tp_string>(first_node);
      auto print = VertexAdaptor<op_print>::create(first_node);
      set_location(print, print_location);
      res = print;
      break;
    }

    case tok_exit:
      res = get_exit();
      break;
    case tok_require:
      res = get_require();
      break;
    case tok_require_once:
      res = get_require_once();
      break;

    case tok_constructor_call:
      res = get_func_call<op_constructor_call, op_none>();
      break;
    case tok_func_name: {
      bool was_arrow = (*(cur - 1))->type() == tok_arrow;
      cur++;
      if (!test_expect(tok_oppar)) {
        cur--;
        auto v = VertexAdaptor<op_func_name>::create();
        set_location(v, AutoLocation(this));
        next_cur();
        v->str_val = static_cast<string>((*op)->str_val);
        res = v;
        return_flag = was_arrow;
        break;
      }
      cur--;
      res = get_func_call<op_func_call, op_err>();
      return_flag = was_arrow;
      break;
    }
    case tok_function: {
      res = get_anonymous_function();
      break;
    }
    case tok_isset:
      res = get_func_call<op_isset, op_err>();
      break;
    case tok_array:
      res = get_func_call<op_array, op_none>();
      break;
    case tok_tuple:
      res = get_func_call<op_tuple, op_err>();
      CE (!kphp_error(res.as<op_tuple>()->size(), "tuple() must have at least one argument"));
      break;
    case tok_opbrk:
      res = get_short_array();
      break;
    case tok_list:
      res = get_func_call<op_list_ce, op_lvalue_null>();
      break;
    case tok_defined:
      res = get_func_call<op_defined, op_err>();
      break;
    case tok_min: {
      VertexAdaptor<op_min> min_v = get_func_call<op_min, op_err>();
      VertexRange args = min_v->args();
      if (args.size() == 1) {
        args[0] = GenTree::conv_to(args[0], tp_array);
      }
      res = min_v;
      break;
    }
    case tok_max: {
      VertexAdaptor<op_max> max_v = get_func_call<op_max, op_err>();
      VertexRange args = max_v->args();
      if (args.size() == 1) {
        args[0] = GenTree::conv_to(args[0], tp_array);
      }
      res = max_v;
      break;
    }

    case tok_oppar:
      next_cur();
      res = get_expression();
      CE (!kphp_error(res, "Failed to parse expression after '('"));
      res->parent_flag = true;
      CE (expect(tok_clpar, "')'"));
      return_flag = (*cur)->type() != tok_arrow;
      break;
    case tok_str_begin:
      res = get_string_build();
      break;
    default:
      return VertexPtr();
  }

  if (return_flag) {
    return res;
  }

  res = get_postfix_expression(res);
  return res;
}

VertexPtr GenTree::create_ternary_op_vertex(VertexPtr left, VertexPtr right, VertexPtr third) {
  if (right) {
    return VertexAdaptor<op_ternary>::create(left, right, third);
  }

  string left_name = gen_shorthand_ternary_name(stage::get_file());
  auto left_var = VertexAdaptor<op_var>::create();
  left_var->str_val = left_name;
  left_var->extra_type = op_ex_var_superlocal;

  auto left_set = VertexAdaptor<op_set>::create(left_var, left);
  auto left_set_bool = conv_to<tp_bool>(left_set);

  auto left_var_copy = VertexAdaptor<op_var>::create();
  left_var_copy->str_val = left_name;
  left_var_copy->extra_type = op_ex_var_superlocal;

  auto left_var_move = VertexAdaptor<op_move>::create(left_var_copy);
  return VertexAdaptor<op_ternary>::create(left_set_bool, left_var_move, third);
}

VertexPtr GenTree::get_unary_op(int op_priority_cur, Operation unary_op_tp, bool till_ternary) {
  AutoLocation expr_location(this);
  next_cur();

  VertexPtr left = get_binary_op(op_priority_cur, till_ternary);
  if (!left) {
    return VertexPtr();
  }

  if (unary_op_tp == op_log_not) {
    left = conv_to<tp_bool>(left);
  }
  if (unary_op_tp == op_not) {
    left = conv_to<tp_int>(left);
  }
  VertexPtr expr = create_vertex(unary_op_tp, left);
  set_location(expr, expr_location);
  return expr;
}

VertexPtr GenTree::get_binary_op(int op_priority_cur, bool till_ternary) {
  op_priority_cur = std::min(op_priority_cur, OpInfo::op_priority_end);
  if (op_priority_cur == OpInfo::op_priority_end) {
    return get_expr_top();
  }

  if (cur != end) {
    const Operation unary_op_tp = OpInfo::tok_to_unary_op[(*cur)->type()];
    if (unary_op_tp != op_err && OpInfo::priority(unary_op_tp) <= op_priority_cur) {
      return get_unary_op(op_priority_cur, unary_op_tp, till_ternary);
    }
  }

  const bool ternary = op_priority_cur == OpInfo::ternaryP;
  VertexPtr left = get_binary_op(op_priority_cur + 1, till_ternary);
  if (!left || (ternary && till_ternary)) {
    return left;
  }

  bool need = true;
  while (need && cur != end) {
    const Operation binary_op_tp = OpInfo::tok_to_binary_op[(*cur)->type()];
    if (binary_op_tp == op_err || OpInfo::priority(binary_op_tp) != op_priority_cur) {
      break;
    }

    AutoLocation expr_location(this);
    const bool left_to_right = OpInfo::fixity(binary_op_tp) == left_opp;

    next_cur();
    VertexPtr right = ternary
                      ? get_expression()
                      : get_binary_op(op_priority_cur + left_to_right,
                                      till_ternary && op_priority_cur >= OpInfo::ternaryP);
    if (!right && !ternary) {
      kphp_error (0, format("Failed to parse second argument in [%s]", OpInfo::str(binary_op_tp).c_str()));
      return VertexPtr();
    }

    VertexPtr third;
    if (ternary) {
      CE (expect(tok_colon, "':'"));
      third = get_expression_impl(true);
      if (!third) {
        kphp_error (0, format("Failed to parse third argument in [%s]", OpInfo::str(binary_op_tp).c_str()));
        return VertexPtr();
      }
      if (right) {
        left = conv_to<tp_bool>(left);
      }
    }

    if (vk::any_of_equal(binary_op_tp, op_log_or, op_log_and, op_log_or_let, op_log_and_let, op_log_xor_let)) {
      left = conv_to<tp_bool>(left);
      right = conv_to<tp_bool>(right);
    }
    if (vk::any_of_equal(binary_op_tp, op_set_or, op_set_and, op_set_xor, op_set_shl, op_set_shr)) {
      right = conv_to<tp_int>(right);
    }
    if (vk::any_of_equal(binary_op_tp, op_or, op_and, op_xor)) {
      left = conv_to<tp_int>(left);
      right = conv_to<tp_int>(right);
    }

    VertexPtr expr = ternary
                     ? create_ternary_op_vertex(left, right, third)
                     : create_vertex(binary_op_tp, left, right);

    set_location(expr, expr_location);
    left = expr;
    need = need && (left_to_right || ternary);
  }
  return left;
}

VertexPtr GenTree::get_expression_impl(bool till_ternary) {
  return get_binary_op(OpInfo::op_priority_begin, till_ternary);
}

VertexPtr GenTree::get_expression() {
  skip_phpdoc_tokens();
  return get_expression_impl(false);
}

VertexPtr GenTree::embrace(VertexPtr v) {
  if (v->type() != op_seq) {
    auto brace = VertexAdaptor<op_seq>::create(v);
    ::set_location(brace, v->get_location());
    return brace;
  }

  return v;
}

VertexPtr GenTree::get_def_value() {
  VertexPtr val;

  if ((*cur)->type() == tok_eq1) {
    next_cur();
    val = get_expression();
    kphp_error (val, "Cannot parse function parameter");
  }

  return val;
}

VertexPtr GenTree::get_func_param_without_callbacks(bool from_callback) {
  AutoLocation st_location(this);
  Token *tok_type_declaration = nullptr;
  if ((*cur)->type() == tok_func_name || (*cur)->type() == tok_Exception) {
    tok_type_declaration = *cur;
    next_cur();
  }

  VertexPtr name = get_var_name_ref();
  if (!name) {
    return VertexPtr();
  }

  vector<VertexPtr> next;
  next.push_back(name);

  PrimitiveType tp = tp_Unknown;
  VertexPtr type_rule;
  if (!from_callback && (*cur)->type() == tok_triple_colon) {
    tp = get_func_param_type_help();    // запишется в param->type_help, и при вызове будет неявный cast
  } else {
    type_rule = get_type_rule();
  }

  VertexPtr def_val = get_def_value();
  if (def_val) {
    next.push_back(def_val);
  }
  auto v = VertexAdaptor<op_func_param>::create(next);
  set_location(v, st_location);
  if (tok_type_declaration != nullptr) {
    v->type_declaration = static_cast<string>(tok_type_declaration->str_val);
    v->type_help = tok_type_declaration->type() == tok_Exception ? tp_Exception : tp_Class;
  }

  if (type_rule) {
    v->type_rule = type_rule;
  } else if (tp != tp_Unknown) {
    v->type_help = tp;
  }

  return v;
}

VertexPtr GenTree::get_func_param_from_callback() {
  return get_func_param_without_callbacks(true);
}

VertexPtr GenTree::get_func_param() {
  AutoLocation st_location(this);
  if (test_expect(tok_func_name) && (*(cur + 1))->type() == tok_oppar) { // callback
    auto name = VertexAdaptor<op_func_name>::create();
    set_location(name, st_location);
    name->str_val = static_cast<string>((*cur)->str_val);
    kphp_assert(name->str_val == "callback");
    next_cur();

    CE (expect(tok_oppar, "'('"));
    std::vector<VertexPtr> callback_params;
    bool ok_params_next = gen_list<op_err>(&callback_params, &GenTree::get_func_param_from_callback, tok_comma);
    CE (!kphp_error(ok_params_next, "Failed to parse callback params"));
    auto params = VertexAdaptor<op_func_param_list>::create(callback_params);
    set_location(params, st_location);
    CE (expect(tok_clpar, "')'"));

    VertexPtr type_rule = get_type_rule();

    VertexPtr def_val = get_def_value();
    kphp_assert(!def_val || (def_val->type() == op_func_name && def_val->get_string() == "TODO"));

    VertexPtr v;
    if (def_val) {
      v = VertexAdaptor<op_func_param_callback>::create(name, params, def_val);
    } else {
      v = VertexAdaptor<op_func_param_callback>::create(name, params);
    }

    v->type_rule = type_rule;
    set_location(v, st_location);

    return v;
  }

  return get_func_param_without_callbacks();
}

VertexPtr GenTree::get_foreach_param() {
  AutoLocation location(this);
  VertexPtr xs = get_expression();
  CE (!kphp_error(xs, ""));

  CE (expect(tok_as, "'as'"));
  skip_phpdoc_tokens();

  VertexPtr x, key;
  x = get_var_name_ref();
  CE (!kphp_error(x, ""));
  if ((*cur)->type() == tok_double_arrow) {
    next_cur();
    key = x;
    x = get_var_name_ref();
    CE (!kphp_error(x, ""));
  }

  vector<VertexPtr> next;
  next.push_back(xs);
  next.push_back(x);
  auto empty = VertexAdaptor<op_empty>::create();
  next.push_back(empty); // will be replaced
  if (key) {
    next.push_back(key);
  }
  auto res = VertexAdaptor<op_foreach_param>::create(next);
  set_location(res, location);
  return res;
}

VertexPtr GenTree::conv_to(VertexPtr x, PrimitiveType tp, int ref_flag) {
  if (ref_flag) {
    switch (tp) {
      case tp_array:
        return conv_to_lval<tp_array>(x);
      case tp_int:
        return conv_to_lval<tp_int>(x);
      case tp_var:
        return x;
        break;
      default:
        kphp_error (0, "convert_to not array with ref_flag");
        return x;
    }
  }
  switch (tp) {
    case tp_int:
      return conv_to<tp_int>(x);
    case tp_bool:
      return conv_to<tp_bool>(x);
    case tp_string:
      return conv_to<tp_string>(x);
    case tp_float:
      return conv_to<tp_float>(x);
    case tp_array:
      return conv_to<tp_array>(x);
    case tp_UInt:
      return conv_to<tp_UInt>(x);
    case tp_Long:
      return conv_to<tp_Long>(x);
    case tp_ULong:
      return conv_to<tp_ULong>(x);
    case tp_regexp:
      return conv_to<tp_regexp>(x);
    default:
      return x;
  }
}

VertexPtr GenTree::get_actual_value(VertexPtr v) {
  if (v->type() == op_var && v->extra_type == op_ex_var_const && v->get_var_id()) {
    return v->get_var_id()->init_val;
  }
  if (v->type() == op_define_val) {
    DefinePtr d = v.as<op_define_val>()->define_id;
    if (d->type() == DefineData::def_const) {
      return d->val;
    }
  }
  return v;
}

PrimitiveType GenTree::get_ptype() {
  PrimitiveType tp;
  TokenType tok = (*cur)->type();
  switch (tok) {
    case tok_int:
      tp = tp_int;
      break;
    case tok_string:
      tp = tp_string;
      break;
    case tok_float:
      tp = tp_float;
      break;
    case tok_array:
      tp = tp_array;
      break;
    case tok_bool:
      tp = tp_bool;
      break;
    case tok_var:
      tp = tp_var;
      break;
    case tok_Exception:
      tp = tp_Exception;
      break;
    case tok_tuple:
      tp = tp_tuple;
      break;
    case tok_func_name:
      tp = get_ptype_by_name(static_cast<string>((*cur)->str_val));
      break;
    default:
      tp = tp_Error;
      break;
  }
  if (tp != tp_Error) {
    next_cur();
  }
  return tp;
}

PrimitiveType GenTree::get_func_param_type_help() {
  kphp_assert((*cur)->type() == tok_triple_colon);

  next_cur();
  PrimitiveType res = get_ptype();
  kphp_error (res != tp_Error, "Cannot parse type");

  return res;
}

VertexPtr GenTree::get_type_rule_func() {
  AutoLocation rule_location(this);
  string name{(*cur)->str_val};
  next_cur();
  CE (expect(tok_lt, "<"));
  vector<VertexPtr> next;
  bool ok_next = gen_list<op_err>(&next, &GenTree::get_type_rule_, tok_comma);
  CE (!kphp_error(ok_next, "Failed get_type_rule_func"));
  CE (expect(tok_gt, ">"));

  auto rule = VertexAdaptor<op_type_rule_func>::create(next);
  set_location(rule, rule_location);
  rule->str_val = name;
  return rule;
}

VertexPtr GenTree::get_type_rule_() {
  PrimitiveType tp = get_ptype();
  TokenType tok = (*cur)->type();
  VertexPtr res = VertexPtr(nullptr);
  if (tp != tp_Error) {
    AutoLocation arr_location(this);

    vector<VertexPtr> next;
    if (tok == tok_lt) {        // array<...>, tuple<...,...>
      kphp_error(tp == tp_array || tp == tp_tuple, "Cannot parse type_rule");
      bool allow_comma_sep = tp == tp_tuple;
      do {
        next_cur();
        next.push_back(get_type_rule_());
        CE (!kphp_error(next.back(), "Cannot parse type_rule (1)"));
      } while (allow_comma_sep && test_expect(tok_comma));
      CE (expect(tok_gt, "'>'"));
    }

    auto arr = VertexAdaptor<op_type_rule>::create(next);
    arr->type_help = tp;
    set_location(arr, arr_location);
    res = arr;
  } else if (tok == tok_func_name) {
    if ((*cur)->str_val == "lca" || (*cur)->str_val == "OrFalse") {
      res = get_type_rule_func();
    } else if ((*cur)->str_val == "self") {
      auto self = VertexAdaptor<op_self>::create();
      res = self;
    } else if ((*cur)->str_val == "CONST") {
      next_cur();
      res = get_type_rule_();
      if (res) {
        res->extra_type = op_ex_rule_const;
      }
    } else {
      kphp_error(0, format("Can't parse type_rule. Unknown string [%s]", string((*cur)->str_val).c_str()));
    }
  } else if (tok == tok_xor) {
    next_cur();
    if (kphp_error (test_expect(tok_int_const), "Int expected")) {
      return VertexPtr();
    }
    auto v = VertexAdaptor<op_arg_ref>::create();
    set_location(v, AutoLocation(this));
    v->int_val = std::atoi(std::string((*cur)->str_val).c_str());
    res = v;
    next_cur();
    while (test_expect(tok_opbrk)) {
      AutoLocation opbrk_location(this);
      next_cur();
      CE (expect(tok_clbrk, "]"));
      auto index = VertexAdaptor<op_index>::create(res);
      set_location(index, opbrk_location);
      res = index;
    }

    if (test_expect(tok_oppar)) {
      AutoLocation oppar_location(this);
      next_cur();
      CE (expect(tok_clpar, ")"));
      auto call = VertexAdaptor<op_type_rule_func>::create(res);
      call->set_string("callback_call");
      set_location(call, oppar_location);
      res = call;
    }
  }
  return res;
}

VertexPtr GenTree::get_type_rule() {
  VertexPtr res, first;

  TokenType tp = (*cur)->type();
  if (vk::any_of_equal(tp, tok_triple_colon, tok_triple_eq, tok_triple_lt, tok_triple_gt)) {
    AutoLocation rule_location(this);
    next_cur();
    first = get_type_rule_();
    CE(!kphp_error(first, "Cannot parse type rule"));

    VertexPtr rule = create_vertex(OpInfo::tok_to_op[tp], first);
    set_location(rule, rule_location);
    res = rule;
  }
  return res;
}

void GenTree::func_force_return(VertexPtr root, VertexPtr val) {
  if (root->type() != op_function) {
    return;
  }
  VertexAdaptor<op_function> func = root;

  VertexPtr cmd = func->cmd();
  assert (cmd->type() == op_seq);

  bool no_result = !val;
  if (no_result) {
    auto return_val = VertexAdaptor<op_null>::create();
    val = return_val;
  }

  auto return_node = VertexAdaptor<op_return>::create(val);
  return_node->void_flag = no_result;
  vector<VertexPtr> next = cmd->get_next();
  next.push_back(return_node);
  auto seq = VertexAdaptor<op_seq>::create(next);
  func->cmd() = seq;
}

VertexPtr GenTree::create_vertex_this(const AutoLocation &location, ClassPtr cur_class, bool with_type_rule) {
  auto this_var = VertexAdaptor<op_var>::create();
  this_var->str_val = "this";
  this_var->extra_type = op_ex_var_this;
  this_var->const_type = cnst_const_val;
  set_location(this_var, location);

  if (with_type_rule) {
    kphp_assert(cur_class);

    auto rule_this_var = VertexAdaptor<op_class_type_rule>::create();
    rule_this_var->type_help = tp_Class;
    rule_this_var->class_ptr = cur_class;

    this_var->type_rule = VertexAdaptor<op_common_type_rule>::create(rule_this_var);
  }

  return this_var;
}

// __construct(args) { body } => __construct(args) { $this ::: tp_Class; def vars init; body; return $this; }
void GenTree::patch_func_constructor(VertexAdaptor<op_function> func, ClassPtr cur_class, AutoLocation location) {
  auto return_node = VertexAdaptor<op_return>::create(create_vertex_this(location, cur_class));
  set_location(return_node, location);

  std::vector<VertexPtr> next = func->cmd()->get_next();
  next.insert(next.begin(), create_vertex_this(location, cur_class, true));

  // выносим "$var = 0" в начало конструктора; переменные класса — в порядке, обратном объявлению, это не страшно
  cur_class->members.for_each([&](ClassMemberInstanceField &f) {
    if (f.root->has_def_val()) {
      auto inst_prop = VertexAdaptor<op_instance_prop>::create(create_vertex_this(location, ClassPtr()));
      set_location(inst_prop, location);
      inst_prop->str_val = f.root->get_string();

      next.insert(next.begin() + 1, VertexAdaptor<op_set>::create(inst_prop, f.root->def_val()));
    }
  });

  next.emplace_back(return_node);

  func->cmd() = VertexAdaptor<op_seq>::create(next);
}

// function fname(args) => function fname($this ::: class_instance, args)
void GenTree::patch_func_add_this(vector<VertexPtr> &params_next, const AutoLocation &func_location, ClassPtr cur_class) {
  params_next.emplace_back(VertexAdaptor<op_func_param>::create(create_vertex_this(func_location, cur_class, true)));
}

void GenTree::create_default_constructor(ClassPtr cur_class, AutoLocation location) const {
  create_constructor_with_args(cur_class, location, VertexAdaptor<op_func_param_list>::create(), parsed_os);
}

void GenTree::create_constructor_with_args(ClassPtr cur_class,
                                           AutoLocation location, VertexAdaptor<op_func_param_list> params,
                                           DataStream<FunctionPtr> &os) {
  auto func_name = VertexAdaptor<op_func_name>::create();
  func_name->str_val = replace_backslashes(cur_class->name) + "$$" + "__construct";

  std::vector<VertexPtr> fields_initializers;
  for (auto param : params->params()) {
    param = param.as<op_func_param>()->var();
    auto inst_prop = VertexAdaptor<op_instance_prop>::create(create_vertex_this(location, ClassPtr()));
    set_location(inst_prop, location);
    inst_prop->str_val = param->get_string();

    fields_initializers.emplace_back(VertexAdaptor<op_set>::create(inst_prop, param.clone()));
  }
  auto func_root = VertexAdaptor<op_seq>::create(fields_initializers);

  auto func = VertexAdaptor<op_function>::create(func_name, params, func_root);
  func->inline_flag = true;
  func->location.line = location.line_num;

  patch_func_constructor(func, cur_class, location);

  FunctionPtr ctor_function = FunctionData::create_function(func, FunctionData::func_local);

  cur_class->members.add_instance_method(ctor_function, access_public);

  G->register_and_require_function(ctor_function, os, true);    // instance-методы force require
}

template<Operation Op>
VertexPtr GenTree::get_multi_call(GetFunc f) {
  TokenType type = (*cur)->type();
  AutoLocation seq_location(this);
  next_cur();

  vector<VertexPtr> next;
  bool ok_next = gen_list<op_err>(&next, f, tok_comma);
  CE (!kphp_error(ok_next, "Failed get_multi_call"));

  for (int i = 0, ni = (int)next.size(); i < ni; i++) {
    if (type == tok_echo || type == tok_dbg_echo) {
      next[i] = conv_to<tp_string>(next[i]);
    }
    auto v = VertexAdaptor<Op>::create(next[i]);
    ::set_location(v, next[i]->get_location());
    next[i] = v;
  }
  auto seq = VertexAdaptor<op_seq>::create(next);
  set_location(seq, seq_location);
  return seq;
}


VertexPtr GenTree::get_return() {
  AutoLocation ret_location(this);
  next_cur();
  skip_phpdoc_tokens();
  VertexPtr return_val = get_expression();
  bool no_result = false;
  if (!return_val) {
    auto tmp = VertexAdaptor<op_null>::create();
    set_location(tmp, AutoLocation(this));
    return_val = tmp;
    no_result = true;
  }
  auto ret = VertexAdaptor<op_return>::create(return_val);
  set_location(ret, ret_location);
  CE (expect(tok_semicolon, "';'"));
  ret->void_flag = no_result;
  return ret;
}

VertexPtr GenTree::get_exit() {
  AutoLocation exit_location(this);
  next_cur();
  bool is_opened = open_parent();
  VertexPtr exit_val;
  if (is_opened) {
    exit_val = get_expression();
    close_parent (is_opened);
  }
  if (!exit_val) {
    auto tmp = VertexAdaptor<op_int_const>::create();
    tmp->str_val = "0";
    exit_val = tmp;
  }
  auto v = VertexAdaptor<op_exit>::create(exit_val);
  set_location(v, exit_location);
  return v;
}

template<Operation Op>
VertexPtr GenTree::get_break_continue() {
  AutoLocation res_location(this);
  next_cur();
  VertexPtr first_node = get_expression();
  CE (expect(tok_semicolon, "';'"));

  if (!first_node) {
    auto one = VertexAdaptor<op_int_const>::create();
    one->str_val = "1";
    first_node = one;
  }

  auto res = VertexAdaptor<Op>::create(first_node);
  set_location(res, res_location);
  return res;
}

VertexPtr GenTree::get_foreach() {
  AutoLocation foreach_location(this);
  next_cur();

  CE (expect(tok_oppar, "'('"));
  skip_phpdoc_tokens();
  VertexPtr first_node = get_foreach_param();
  CE (!kphp_error(first_node, "Failed to parse 'foreach' params"));

  CE (expect(tok_clpar, "')'"));

  VertexPtr second_node = get_statement();
  CE (!kphp_error(second_node, "Failed to parse 'foreach' body"));

  auto temp_node = VertexAdaptor<op_empty>::create();

  auto foreach = VertexAdaptor<op_foreach>::create(first_node, embrace(second_node), temp_node);
  set_location(foreach, foreach_location);
  return foreach;
}

VertexPtr GenTree::get_while() {
  AutoLocation while_location(this);
  next_cur();
  CE (expect(tok_oppar, "'('"));
  skip_phpdoc_tokens();
  VertexPtr first_node = get_expression();
  CE (!kphp_error(first_node, "Failed to parse 'while' condition"));
  first_node = conv_to<tp_bool>(first_node);
  CE (expect(tok_clpar, "')'"));

  VertexPtr second_node = get_statement();
  CE (!kphp_error(second_node, "Failed to parse 'while' body"));

  auto while_vertex = VertexAdaptor<op_while>::create(first_node, embrace(second_node));
  set_location(while_vertex, while_location);
  return while_vertex;
}

VertexPtr GenTree::get_if() {
  AutoLocation if_location(this);
  VertexPtr if_vertex;
  next_cur();
  CE (expect(tok_oppar, "'('"));
  skip_phpdoc_tokens();
  VertexPtr first_node = get_expression();
  CE (!kphp_error(first_node, "Failed to parse 'if' condition"));
  first_node = conv_to<tp_bool>(first_node);
  CE (expect(tok_clpar, "')'"));

  VertexPtr second_node = get_statement();
  CE (!kphp_error(second_node, "Failed to parse 'if' body"));
  second_node = embrace(second_node);

  VertexPtr third_node = VertexPtr();
  if ((*cur)->type() == tok_else) {
    next_cur();
    third_node = get_statement();
    CE (!kphp_error(third_node, "Failed to parse 'else' statement"));
  }

  if (third_node) {
    third_node = embrace(third_node);
    auto v = VertexAdaptor<op_if>::create(first_node, second_node, third_node);
    if_vertex = v;
  } else {
    auto v = VertexAdaptor<op_if>::create(first_node, second_node);
    if_vertex = v;
  }
  set_location(if_vertex, if_location);
  return if_vertex;
}

VertexPtr GenTree::get_for() {
  AutoLocation for_location(this);
  next_cur();
  CE (expect(tok_oppar, "'('"));
  skip_phpdoc_tokens();

  AutoLocation pre_cond_location(this);
  vector<VertexPtr> first_next;
  bool ok_first_next = gen_list<op_err>(&first_next, &GenTree::get_expression, tok_comma);
  CE (!kphp_error(ok_first_next, "Failed to parse 'for' precondition"));
  auto pre_cond = VertexAdaptor<op_seq>::create(first_next);
  set_location(pre_cond, pre_cond_location);

  CE (expect(tok_semicolon, "';'"));

  AutoLocation cond_location(this);
  vector<VertexPtr> second_next;
  bool ok_second_next = gen_list<op_err>(&second_next, &GenTree::get_expression, tok_comma);
  CE (!kphp_error(ok_second_next, "Failed to parse 'for' action"));
  if (second_next.empty()) {
    auto v_true = VertexAdaptor<op_true>::create();
    second_next.push_back(v_true);
  } else {
    second_next.back() = conv_to<tp_bool>(second_next.back());
  }
  auto cond = VertexAdaptor<op_seq_comma>::create(second_next);
  set_location(cond, cond_location);

  CE (expect(tok_semicolon, "';'"));

  AutoLocation post_cond_location(this);
  vector<VertexPtr> third_next;
  bool ok_third_next = gen_list<op_err>(&third_next, &GenTree::get_expression, tok_comma);
  CE (!kphp_error(ok_third_next, "Failed to parse 'for' postcondition"));
  auto post_cond = VertexAdaptor<op_seq>::create(third_next);
  set_location(post_cond, post_cond_location);

  CE (expect(tok_clpar, "')'"));

  VertexPtr cmd = get_statement();
  CE (!kphp_error(cmd, "Failed to parse 'for' statement"));

  cmd = embrace(cmd);
  auto for_vertex = VertexAdaptor<op_for>::create(pre_cond, cond, post_cond, cmd);
  set_location(for_vertex, for_location);
  return for_vertex;
}

VertexPtr GenTree::get_do() {
  AutoLocation do_location(this);
  next_cur();
  VertexPtr first_node = get_statement();
  CE (!kphp_error(first_node, "Failed to parser 'do' condition"));

  CE (expect(tok_while, "'while'"));

  CE (expect(tok_oppar, "'('"));
  skip_phpdoc_tokens();
  VertexPtr second_node = get_expression();
  CE (!kphp_error(second_node, "Faild to parse 'do' statement"));
  second_node = conv_to<tp_bool>(second_node);
  CE (expect(tok_clpar, "')'"));
  CE (expect(tok_semicolon, "';'"));

  auto do_vertex = VertexAdaptor<op_do>::create(second_node, first_node);
  set_location(do_vertex, do_location);
  return do_vertex;
}

VertexPtr GenTree::get_switch() {
  AutoLocation switch_location(this);
  vector<VertexPtr> switch_next;

  next_cur();
  CE (expect(tok_oppar, "'('"));
  skip_phpdoc_tokens();
  VertexPtr switch_val = get_expression();
  CE (!kphp_error(switch_val, "Failed to parse 'switch' expression"));
  switch_next.push_back(switch_val);
  CE (expect(tok_clpar, "')'"));

  CE (expect(tok_opbrc, "'{'"));

  // they will be replaced by vars later.
  // It can't be done now, gen_name is not working here.
  auto temp_ver1 = VertexAdaptor<op_empty>::create();
  switch_next.push_back(temp_ver1);
  auto temp_ver2 = VertexAdaptor<op_empty>::create();
  switch_next.push_back(temp_ver2);
  auto temp_ver3 = VertexAdaptor<op_empty>::create();
  switch_next.push_back(temp_ver3);
  auto temp_ver4 = VertexAdaptor<op_empty>::create();
  switch_next.push_back(temp_ver4);

  while ((*cur)->type() != tok_clbrc) {
    skip_phpdoc_tokens();
    TokenType cur_type = (*cur)->type();
    VertexPtr case_val;

    AutoLocation case_location(this);
    if (cur_type == tok_case) {
      next_cur();
      case_val = get_expression();
      CE (!kphp_error(case_val, "Failed to parse 'case' value"));

      CE (expect2(tok_colon, tok_semicolon, "':'"));
    }
    if (cur_type == tok_default) {
      next_cur();
      CE (expect2(tok_colon, tok_semicolon, "':'"));
    }

    AutoLocation seq_location(this);
    vector<VertexPtr> seq_next;
    while (cur != end) {
      if ((*cur)->type() == tok_clbrc) {
        break;
      }
      if ((*cur)->type() == tok_case) {
        break;
      }
      if ((*cur)->type() == tok_default) {
        break;
      }
      VertexPtr cmd = get_statement();
      if (cmd) {
        seq_next.push_back(cmd);
      }
    }

    auto seq = VertexAdaptor<op_seq>::create(seq_next);
    set_location(seq, seq_location);
    if (cur_type == tok_case) {
      auto case_block = VertexAdaptor<op_case>::create(case_val, seq);
      set_location(case_block, case_location);
      switch_next.push_back(case_block);
    } else if (cur_type == tok_default) {
      auto case_block = VertexAdaptor<op_default>::create(seq);
      set_location(case_block, case_location);
      switch_next.push_back(case_block);
    }
  }

  auto switch_vertex = VertexAdaptor<op_switch>::create(switch_next);
  set_location(switch_vertex, switch_location);

  CE (expect(tok_clbrc, "'}'"));
  return switch_vertex;
}

bool GenTree::parse_function_specifiers(VertexPtr flags) {
  switch ((*cur)->type()) {
    case tok_throws: {
      flags->throws_flag = true;
      if (!expect(tok_throws, "'throws'")) {
        return false;
      }
      break;
    }

    case tok_resumable: {
      flags->resumable_flag = true;
      if (!expect(tok_resumable, "'resumable'")) {
        return false;
      }
      break;
    }

    case tok_auto: {
      flags->auto_flag = true;
      if (!expect(tok_auto, "'auto'")) {
        return false;
      }
      break;
    }

    default:
      return true;
  }

  return parse_function_specifiers(flags);
}

bool GenTree::parse_function_uses(std::vector<VertexPtr> *uses_of_lambda) {
  if (test_expect(tok_use)) {
    kphp_error_act(uses_of_lambda, "Unexpected `use` token", return false);

    next_cur();
    if (!expect(tok_oppar, "`(`")) {
      return false;
    }

    bool ok_params_next = gen_list<op_err>(uses_of_lambda, &GenTree::get_var_name_ref, tok_comma);
    for (auto &v : *uses_of_lambda) {
      kphp_error(!v->ref_flag, "references to variables in `use` block are forbidden in lambdas");
      v = VertexAdaptor<op_func_param>::create(v);
    }

    return ok_params_next && expect(tok_clpar, "`)`");
  }

  return true;
}

bool GenTree::check_uses_and_args_are_not_intersect(const std::vector<VertexPtr> &uses, const std::vector<VertexPtr> &params) {
  std::set<std::string> uniq_uses;
  std::transform(uses.begin(), uses.end(),
                 std::inserter(uniq_uses, uniq_uses.begin()),
                 [](VertexPtr v) { return v.as<op_func_param>()->var()->get_string(); });

  return std::none_of(params.begin(), params.end(),
    [&](VertexPtr p) { return uniq_uses.find(p.as<op_func_param>()->var()->get_string()) != uniq_uses.end(); });
}

VertexPtr GenTree::get_anonymous_function() {
  std::vector<VertexPtr> uses_of_lambda;
  VertexPtr f = get_function(nullptr, access_nonmember, &uses_of_lambda);

  if (auto anon_function = f.try_as<op_function>()) {
    // это constructor call
    return generate_anonymous_class(anon_function, parsed_os, cur_function, std::move(uses_of_lambda), processing_file);
  }

  return {};
}

VertexPtr GenTree::parse_function_declaration(AccessType access_type,
                                              std::vector<VertexPtr> *uses_of_lambda,
                                              VertexAdaptor<op_func_param_list> &params,
                                              VertexPtr &flags,
                                              bool &is_constructor) {
  AutoLocation func_location(this);
  set_location(flags, func_location);

  kphp_assert(test_expect(tok_function) || test_expect(tok_ex_function));
  next_cur();

  auto name = VertexAdaptor<op_func_name>::create();
  set_location(name, func_location);

  if (uses_of_lambda != nullptr) {
    name->str_val = gen_anonymous_function_name(processing_file);
  } else {
    CE(expect(tok_func_name, "'tok_func_name'"));
    name->str_val = static_cast<string>((*std::prev(cur))->str_val);
    if (cur_class) {        // fname внутри класса — это full$class$name$$fname
      name->str_val = replace_backslashes(cur_class->name) + "$$" + name->str_val;
    }
  }

  bool is_instance_method = FunctionData::is_instance_access_type(access_type);
  is_constructor = is_instance_method && vk::string_view(name->str_val).ends_with("$$__construct");

  CE(expect(tok_oppar, "'('"));

  AutoLocation params_location(this);
  vector<VertexPtr> params_next;

  if (is_instance_method && !is_constructor) {
    patch_func_add_this(params_next, func_location, cur_class);
  }

  if (test_expect(tok_varg)) {
    flags->varg_flag = true;
    next_cur();
  } else {
    bool ok_params_next = gen_list<op_err>(&params_next, &GenTree::get_func_param, tok_comma);
    CE(!kphp_error(ok_params_next, "Failed to parse function params"));
  }

  params = VertexAdaptor<op_func_param_list>::create(params_next);
  set_location(params, params_location);

  CE(expect(tok_clpar, "')'"));

  CE(parse_function_specifiers(flags));
  CE(parse_function_uses(uses_of_lambda));
  kphp_error(!uses_of_lambda || check_uses_and_args_are_not_intersect(*uses_of_lambda, params_next),
    "arguments and captured variables(in `use` clause) must have different names");

  flags->type_rule = get_type_rule();

  return name;
}

VertexPtr GenTree::get_function(Token *phpdoc_token, AccessType access_type, std::vector<VertexPtr> *uses_of_lambda) {
  bool is_tok_ex_function = (*cur)->type() == tok_ex_function;  // extern_function debugServerLog (...); (костыль, который когда-нибудь уйдёт)

  vertex_inner<meta_op_base> flags_inner;
  VertexPtr flags(&flags_inner);
  VertexAdaptor<op_func_param_list> params;
  bool is_constructor = false;
  VertexAdaptor<op_func_name> name = parse_function_declaration(access_type, uses_of_lambda, params, flags, is_constructor);
  CE(name);

  if (is_tok_ex_function) {
    CE (expect(tok_semicolon, "';'"));
    VertexPtr root = VertexAdaptor<op_extern_func>::create(name, params);
    root->copy_location_and_flags(*flags);
    G->save_extern_func_header(name->get_string(), root);
    return {};
  }

  FunctionData::func_type_t func_type =
    test_expect(tok_semicolon) ? FunctionData::func_extern :
    in_func_cnt_ == 0 && !in_namespace() ? FunctionData::func_global :
    FunctionData::func_local;

  VertexPtr root = func_type == FunctionData::func_extern
                   ? (VertexPtr)VertexAdaptor<op_func_decl>::create(name, params)
                   : (VertexPtr)VertexAdaptor<op_function>::create(name, params, VertexPtr{});
  root->copy_location_and_flags(*flags);

  StackPushPop<FunctionPtr> f_alive(functions_stack, cur_function, FunctionData::create_function(root, func_type));

  cur_function->phpdoc_token = phpdoc_token;

  const bool kphp_required_flag = phpdoc_token &&
                                  (phpdoc_token->str_val.find("@kphp-required") != std::string::npos ||
                                   phpdoc_token->str_val.find("@kphp-lib-export") != std::string::npos);

  if (cur_class && FunctionData::is_instance_access_type(access_type)) {
    cur_class->members.add_instance_method(cur_function, access_type);
  } else if (cur_class && FunctionData::is_static_access_type(access_type)) {
    cur_class->members.add_static_method(cur_function, access_type);
  }

  if (test_expect(tok_opbrc)) {
    enter_function();
    is_top_of_the_function_ = in_func_cnt_ > 1;
    root.as<op_function>()->cmd() = get_statement();
    exit_function();
    CE(!kphp_error(root.as<op_function>()->cmd(), "Failed to parse function body"));

    if (is_constructor) {
      patch_func_constructor(root, cur_class, AutoLocation(this));
    } else {
      func_force_return(root);
    }
  } else {
    CE (expect(tok_semicolon, "';'"));
  }

  bool auto_require = cur_function->type() == FunctionData::func_global
                      || cur_function->type() == FunctionData::func_extern
                      || cur_function->is_instance_function()
                      || kphp_required_flag;
  G->register_and_require_function(cur_function, parsed_os, auto_require);

  if (uses_of_lambda != nullptr && !stage::has_error()) {
    return root;
  }

  return {};
}

bool GenTree::check_seq_end() {
  if (!test_expect(tok_clbrc)) {
    kphp_error (0, "Failed to parse sequence");
    while (cur != end && !test_expect(tok_clbrc)) {
      next_cur();
    }
  }
  return expect (tok_clbrc, "'}'");
}

bool GenTree::check_statement_end() {
  //if (test_expect (tok_clbrc)) {
  //return true;
  //}
  if (!test_expect(tok_semicolon)) {
    stage::set_line(line_num);
    kphp_error (0, "Failed to parse statement. Expected `;`");
    while (cur != end && !test_expect(tok_clbrc) && !test_expect(tok_semicolon)) {
      next_cur();
    }
  }
  return expect (tok_semicolon, "';'");
}

static inline bool is_class_name_allowed(const string &name) {
  static set<string> disallowed_names{"Exception", "RpcMemcache", "Memcache", "rpc_connection", "Long", "ULong", "UInt", "true_mc", "test_mc", "rich_mc",
                                      "db_decl"};

  return disallowed_names.find(name) == disallowed_names.end();
}

VertexPtr GenTree::get_class(Token *phpdoc_token) {
  AutoLocation class_location(this);
  CE (expect(tok_class, "'class'"));
  CE (!kphp_error(test_expect(tok_func_name), "Class name expected"));

  auto func_name = VertexAdaptor<op_func_name>::create();
  func_name->str_val = processing_file->main_func_name;     // пока что класс == global-функция файла
  auto func_params = VertexAdaptor<op_func_param_list>::create();
  auto func_body = VertexAdaptor<op_seq>::create();
  auto func_root = VertexAdaptor<op_function>::create(func_name, func_params, func_body);

  StackPushPop<FunctionPtr> f_alive(functions_stack, cur_function, FunctionData::create_function(func_root, FunctionData::func_global));
  StackPushPop<ClassPtr> c_alive(class_stack, cur_class, ClassPtr(new ClassData()));

  cur_function->class_id = cur_class;
  cur_class->init_function = cur_function;

  string name_str = static_cast<string>((*cur)->str_val);
  string full_class_name = processing_file->namespace_name.empty() ? name_str : processing_file->namespace_name + "\\" + name_str;
  if (in_namespace()) {
    string expected_name = processing_file->short_file_name;
    kphp_error (name_str == expected_name,
                format("Expected class name %s, found %s", expected_name.c_str(), name_str.c_str()));
  }
  if (!is_class_name_allowed(name_str)) {
    kphp_error (false, format("Sorry, kPHP doesn't support class name %s", name_str.c_str()));
  }

  next_cur();
  if (test_expect(tok_extends)) {
    next_cur();
    CE (!kphp_error(test_expect(tok_func_name), "Class name expected after 'extends'"));
    string full_extends_class_name = resolve_uses(cur_function, static_cast<string>((*cur)->str_val), '\\');
    cur_class->str_dependents.emplace_back(ctype_class, full_extends_class_name);
    next_cur();
  }

  auto name_vertex = VertexAdaptor<op_func_name>::create();
  set_location(name_vertex, AutoLocation(this));
  name_vertex->str_val = name_str;

  auto class_vertex = VertexAdaptor<op_class>::create(name_vertex);
  set_location(class_vertex, class_location);

  cur_class->set_name_and_src_name(full_class_name);    // с полным неймспейсом и слешами
  cur_class->file_id = processing_file;
  cur_class->phpdoc_token = phpdoc_token;
  cur_class->root = class_vertex;

  VertexPtr body_vertex = get_statement();
  CE (!kphp_error(body_vertex, "Failed to parse class body"));

  VertexPtr constant_field_class = generate_constant_field_class(class_vertex);
  cur_class->members.add_constant(constant_field_class);    // A::class это константа на самом деле

  if ((cur_class->members.has_any_instance_var() || cur_class->members.has_any_instance_method()) &&
      !cur_class->members.has_constructor()) {
    create_default_constructor(cur_class, AutoLocation(this));
  }

  vector<VertexPtr> seq;
  cur_class->members.for_each([&seq](ClassMemberConstant &f) {
    seq.emplace_back(f.root);
  });
  cur_class->members.for_each([&seq](ClassMemberStaticField &f) {
    seq.emplace_back(f.root);
  });

  cur_function->root.as<op_function>()->cmd() = VertexAdaptor<op_seq>::create(seq);
  func_force_return(cur_function->root);

  G->register_and_require_function(cur_function, parsed_os, true);  // global-функции всегда require

  return VertexPtr();
}

void GenTree::add_this_to_captured_variables_in_lambda_body(VertexPtr &root, ClassPtr lambda_class) {
  if (root->type() == op_var) {
    if (lambda_class->members.get_instance_field(root->get_string())) {
      auto inst_prop = VertexAdaptor<op_instance_prop>::create(create_vertex_this(AutoLocation(-1), ClassPtr()));
      inst_prop->location = root->location;
      inst_prop->str_val = root->get_string();
      root = inst_prop;
    } else if (root->get_string() == "this") {
      // replace `$this` with `$this->parent$this`
      auto new_root = VertexAdaptor<op_instance_prop>::create(root);
      new_root->set_string("parent$this");
      ::set_location(new_root, root->location);
      root = new_root;
    }
    return;
  }

  for (auto &v : *root) {
    add_this_to_captured_variables_in_lambda_body(v, lambda_class);
  }
}

VertexAdaptor<op_function> GenTree::generate__invoke_method(ClassPtr cur_class, const VertexAdaptor<op_function> &function) {
  auto new_name = VertexAdaptor<op_func_name>::create();
  new_name->set_string("__invoke");
  ::set_location(new_name, function->name()->location);

  // TODO: add parent $this as argument, for implicit capturing parent class
  std::vector<VertexPtr> func_parameters;
  patch_func_add_this(func_parameters, AutoLocation(function->location.line), cur_class);
  auto range = function->params().as<op_func_param_list>()->args();
  if (function->get_func_id()->function_in_which_lambda_was_created || function->get_func_id()->is_lambda()) {
    kphp_assert(range.size() > 0);
    // skip $this parameter, which was added to `function` previously
    func_parameters.insert(func_parameters.end(), range.begin() + 1, range.end());
  } else {
    func_parameters.insert(func_parameters.end(), range.begin(), range.end());
  }

  // every parameter (excluding $this) could be any class_instance
  for (size_t i = 1, id = 0; i < func_parameters.size(); ++i) {
    auto param = func_parameters[i].as<op_func_param>();
    if (param->type_declaration.empty()) {
      param->template_type_id = id++;
    }
  }

  auto params = VertexAdaptor<op_func_param_list>::create(func_parameters);
  params->location.line = function->params()->location.line;

  auto new_cmd = function->cmd().clone();
  if (!function->get_func_id()->function_in_which_lambda_was_created) {
    add_this_to_captured_variables_in_lambda_body(new_cmd, cur_class);
  }

  auto res = VertexAdaptor<op_function>::create(new_name, params, new_cmd);
  res->location = function->location;
  return res;
}

VertexPtr GenTree::generate_constructor_call(ClassPtr cur_class) {
  std::vector<VertexPtr> args;
  cur_class->members.for_each([&](const ClassMemberInstanceField &field) {
    VertexPtr res = VertexAdaptor<op_var>::create();
    if (field.root->get_string() == "parent$this") {
      res->set_string("this");
    } else {
      res->set_string(field.root->get_string());
    }
    res->location = field.root->location;
    args.emplace_back(res);
  });

  auto constructor_call = VertexAdaptor<op_constructor_call>::create(args);
  constructor_call->set_string(cur_class->name);
  constructor_call->set_func_id(cur_class->new_function);

  return constructor_call;
}

VertexPtr GenTree::generate_anonymous_class(VertexAdaptor<op_function> function,
                                            DataStream<FunctionPtr> &os,
                                            FunctionPtr function_in_which_lambda_was_created,
                                            std::vector<VertexPtr> &&uses_of_lambda,
                                            SrcFilePtr file_id) {
  VertexAdaptor<op_func_name> lambda_class_name = VertexAdaptor<op_func_name>::create();
  lambda_class_name->str_val = gen_anonymous_function_name(file_id);
  lambda_class_name->location.line = function->name()->location.line;

  VertexAdaptor<op_class> class_vertex = VertexAdaptor<op_class>::create(lambda_class_name);

  ClassPtr anon_class(new ClassData());
  anon_class->set_name_and_src_name(FunctionData::get_lambda_namespace() + "\\" + lambda_class_name->get_string());
  anon_class->root = class_vertex;

  if (function_in_which_lambda_was_created->is_instance_function()) {
    auto implicit_captured_var_parent_this = VertexAdaptor<op_var>::create();
    implicit_captured_var_parent_this->set_string("parent$this");
    ::set_location(implicit_captured_var_parent_this, lambda_class_name->location);
    auto func_param = VertexAdaptor<op_func_param>::create(implicit_captured_var_parent_this);
    ::set_location(func_param, lambda_class_name->location);

    uses_of_lambda.insert(uses_of_lambda.begin(), func_param);
  }

  for (auto one_use : uses_of_lambda) {
    if (auto param_as_use = one_use.try_as<op_func_param>()) {
      auto variable_in_use = VertexAdaptor<op_class_var>::create();
      variable_in_use->str_val = param_as_use->var()->get_string();
      ::set_location(variable_in_use, param_as_use->location);
      anon_class->members.add_instance_field(variable_in_use, access_public);
    }
  }

  auto register_invoke = [&](VertexAdaptor<op_function> fun, FunctionPtr previous_lambda) {
    std::string s = fun->name()->get_string();
    fun->name()->set_string(replace_backslashes(anon_class->name) + "$$" + s);
    FunctionPtr invoke_function = FunctionData::create_function(fun, FunctionData::func_local);
    invoke_function->function_in_which_lambda_was_created = function_in_which_lambda_was_created;
    anon_class->members.add_instance_method(invoke_function, access_public);
    G->register_and_require_function(invoke_function, os, true);    // instance-методы force require

    auto params = fun->params().as<op_func_param_list>()->args();
    invoke_function->is_template = !uses_of_lambda.empty() || params.size() > 1;
    invoke_function->root->inline_flag = true;

    invoke_function->lambdas_inside = std::move(previous_lambda->lambdas_inside);
    for (auto &l : invoke_function->lambdas_inside) {
      l->function_in_which_lambda_was_created = invoke_function;
    }

    return invoke_function;
  };

  register_invoke(generate__invoke_method(anon_class, function), function->get_func_id());

  auto constructor_params = VertexAdaptor<op_func_param_list>::create(uses_of_lambda);
  ::set_location(constructor_params, lambda_class_name->location);
  create_constructor_with_args(anon_class, AutoLocation(function->location.line), constructor_params, os);
  anon_class->new_function->is_template = !uses_of_lambda.empty();
  anon_class->new_function->function_in_which_lambda_was_created = function_in_which_lambda_was_created;

  G->register_class(anon_class);
  anon_class->init_function = FunctionPtr(new FunctionData());

  auto constructor_call = generate_constructor_call(anon_class);
  constructor_call->location = lambda_class_name->location;
  return constructor_call;
}

VertexPtr GenTree::get_use() {
  kphp_assert(test_expect(tok_use));
  next_cur();
  while (true) {
    if (!test_expect(tok_func_name)) {
      expect(tok_func_name, "<namespace path>");
    }
    string name = static_cast<string>((*cur)->str_val);
    kphp_assert(!name.empty());
    if (name[0] == '\\') {
      name = name.substr(1);
    }
    string alias = name.substr(name.rfind('\\') + 1);
    kphp_error(!alias.empty(), "KPHP doesn't support use of global namespace");
    next_cur();
    if (test_expect(tok_as)) {
      next_cur();
      if (!test_expect(tok_func_name)) {
        expect(tok_func_name, "<use alias>");
      }
      alias = static_cast<string>((*cur)->str_val);
      next_cur();
    }
    if (processing_file->namespace_uses.find(alias) == processing_file->namespace_uses.end()) {
      processing_file->namespace_uses[alias] = name;
    }
    if (!test_expect(tok_comma)) {
      break;
    }
    next_cur();
  }
  expect2(tok_semicolon, tok_comma, "';' or ','");
  return VertexPtr();
}

VertexPtr GenTree::get_namespace_class() {
  kphp_assert (test_expect(tok_namespace));
  kphp_assert (processing_file->namespace_name.empty());
  next_cur();
  kphp_error (test_expect(tok_func_name), "Namespace name expected");
  string namespace_name{(*cur)->str_val};
  std::string real_unified_dir = processing_file->unified_dir_name;
  if (processing_file->owner_lib) {
    vk::string_view current_file_unified_dir = real_unified_dir;
    vk::string_view lib_unified_dir = processing_file->owner_lib->unified_lib_dir();
    kphp_assert_msg(current_file_unified_dir.starts_with(lib_unified_dir), "lib processing file should be in lib dir");
    real_unified_dir.erase(0, lib_unified_dir.size() + 1);
  }
  string expected_namespace_name = replace_characters(real_unified_dir, '/', '\\');
  kphp_error (namespace_name == expected_namespace_name,
              format("Wrong namespace name, expected %s", expected_namespace_name.c_str()));
  processing_file->namespace_name = namespace_name;
  next_cur();
  expect (tok_semicolon, "';'");
  if (stage::has_error()) {
    while (cur != end) {
      ++cur;
    }
    return VertexPtr();
  }
  while (test_expect(tok_use)) {
    get_use();
  }

  Token *phpdoc_token = nullptr;
  if ((*cur)->type() == tok_phpdoc || (*cur)->type() == tok_phpdoc_kphp) {
    phpdoc_token = *cur;
    next_cur();
  }
  VertexPtr cv = get_class(phpdoc_token);
  CE (check_statement_end());
  return cv;
}

AccessType convert_token_type_to_access_type(TokenType access_token, bool is_static) {
  return is_static
         ? (access_token == tok_public ? access_static_public :
            access_token == tok_private ? access_static_private : access_static_protected)
         : (access_token == tok_public ? access_public :
            access_token == tok_private ? access_private : access_protected);
}

VertexPtr GenTree::get_static_field_list(Token *phpdoc_token __attribute__ ((unused)), AccessType access_type) {
  VertexPtr v = get_multi_call<op_static>(&GenTree::get_expression);  // cur сразу перед $field_name
  CE (check_statement_end());

  const OperationExtra extra_type =
    access_type == access_static_private ? op_ex_static_private :
    access_type == access_static_public ? op_ex_static_public : op_ex_static_protected;

  // с учётом переделок, это можно удалить: оно по факту нужно лишь чтобы достать local_name
  for (auto e : *v) {
    kphp_assert(e->type() == op_static);
    e->extra_type = extra_type;
    VertexAdaptor<op_static> seq = e;
    for (auto node : seq->args()) {
      VertexAdaptor<op_var> var;
      if (node->type() == op_var) {
        var = node;
      } else if (node->type() == op_set) {
        VertexAdaptor<op_set> set_expr = node;
        var = set_expr->lhs();
        kphp_error_act (
          var->type() == op_var,
          "unexpected expression in 'static'",
          continue
        );
      } else {
        kphp_error_act (0, "unexpected expression in 'static'", continue);
      }
      cur_class->members.add_static_field(e, var->str_val, access_type);
    }
  }

  return VertexAdaptor<op_empty>::create();
}

VertexPtr GenTree::get_statement(Token *phpdoc_token) {
  VertexPtr res, first_node, second_node, third_node, forth_node, tmp_node;
  TokenType type = (*cur)->type();

  VertexPtr type_rule;
  is_top_of_the_function_ &= vk::any_of_equal(type, tok_global, tok_opbrc);

  switch (type) {
    case tok_class:
      res = get_class(phpdoc_token);
      return VertexPtr();
    case tok_opbrc:
      next_cur();
      res = get_seq();
      kphp_error (res, "Failed to parse sequence");
      CE (check_seq_end());
      return res;
    case tok_return:
      res = get_return();
      return res;
    case tok_continue:
      res = get_break_continue<op_continue>();
      return res;
    case tok_break:
      res = get_break_continue<op_break>();
      return res;
    case tok_unset:
      res = get_func_call<op_unset, op_err>();
      CE (check_statement_end());
      return res;
    case tok_var_dump:
      res = get_func_call<op_var_dump, op_err>();
      CE (check_statement_end());
      return res;
    case tok_define:
      res = get_func_call<op_define, op_err>();
      CE (check_statement_end());
      return res;
    case tok_global:
      if (G->env().get_warnings_level() >= 2 && in_func_cnt_ > 1 && !is_top_of_the_function_) {
        kphp_warning("`global` keyword is allowed only at the top of the function");
      }
      res = get_multi_call<op_global>(&GenTree::get_var_name);
      CE (check_statement_end());
      return res;
    case tok_static:
      if (cur != end && cur + 1 != end) {
        TokenType next_tok = (*(cur + 1))->type();
        if (vk::any_of_equal(next_tok, tok_public, tok_private, tok_protected)) {
          CE (!kphp_error(cur_class, "Access modifier used outside of class"));
          const AccessType access_type = convert_token_type_to_access_type(next_tok, true);
          next_cur();
          next_tok = cur + 1 == end ? tok_end : (*(cur + 1))->type();

          // статическая функция (static public function ...)
          if (next_tok == tok_function) {
            next_cur();
            return get_function(phpdoc_token, access_type);
          }
          // статическое свойство (static public $staticField [, $staticField2...])
          if (next_tok == tok_var_name) {
            return get_static_field_list(phpdoc_token, access_type);
          }
          // ошибочный синтаксис
          CE (!kphp_error(0, "Expected `function` or variable name after access modifier"));
        } else if (vk::none_of_equal(next_tok, tok_function, tok_var_name)) {
          next_cur();
          CE (!kphp_error(0, "Expected `function` or variable name after keyword `static`"));
        }
      }

      res = get_multi_call<op_static>(&GenTree::get_expression);    // static-переменные внутри функций
      CE (check_statement_end());
      return res;
    case tok_echo:
      res = get_multi_call<op_echo>(&GenTree::get_expression);
      CE (check_statement_end());
      return res;
    case tok_dbg_echo:
      res = get_multi_call<op_dbg_echo>(&GenTree::get_expression);
      CE (check_statement_end());
      return res;
    case tok_throw: {
      AutoLocation throw_location(this);
      next_cur();
      first_node = get_expression();
      CE (!kphp_error(first_node, "Empty expression in throw"));
      auto throw_vertex = VertexAdaptor<op_throw>::create(first_node);
      set_location(throw_vertex, throw_location);
      CE (check_statement_end());
      return throw_vertex;
    }

    case tok_while:
      return get_while();
    case tok_if:
      return get_if();
    case tok_for:
      return get_for();
    case tok_do:
      return get_do();
    case tok_foreach:
      return get_foreach();
    case tok_switch:
      return get_switch();
    case tok_protected:
    case tok_public:
    case tok_private: {
      CE (!kphp_error(cur_class, "Access modifier used outside of class"));
      next_cur();
      const TokenType cur_tok = cur == end ? tok_end : (*cur)->type();
      const TokenType next_tok = cur == end || cur + 1 == end ? tok_end : (*(cur + 1))->type();
      const AccessType access_type = convert_token_type_to_access_type(type, cur_tok == tok_static);

      // не статическая функция (public function ...)
      if (cur_tok == tok_function) {
        return get_function(phpdoc_token, access_type);
      }
      // статическая функция (public static function ...)
      if (next_tok == tok_function) {
        expect (tok_static, "'static'");
        return get_function(phpdoc_token, access_type);
      }
      // не статическое свойство (public $var1 [=default] [,$var2...])
      if (cur_tok == tok_var_name) {
        return get_instance_var_list(phpdoc_token, access_type);
      }
      // статическое свойство (public static $staticField [, $staticField2...])
      return get_static_field_list(phpdoc_token, access_type);
    }
    case tok_phpdoc_kphp:
    case tok_phpdoc: {
      Token *token = *cur;
      next_cur();
      return get_statement(token);
    }
    case tok_ex_function:
    case tok_function:
      return get_function(phpdoc_token);

    case tok_try: {
      AutoLocation try_location(this);
      next_cur();
      first_node = get_statement();
      CE (!kphp_error(first_node, "Cannot parse try block"));
      CE (expect(tok_catch, "'catch'"));
      CE (expect(tok_oppar, "'('"));
      CE (expect(tok_Exception, "'Exception'"));
      second_node = get_expression();
      CE (!kphp_error(second_node, "Cannot parse catch ( ??? )"));
      CE (!kphp_error(second_node->type() == op_var, "Expected variable name in 'catch'"));
      second_node->type_help = tp_Exception;

      CE (expect(tok_clpar, "')'"));
      third_node = get_statement();
      CE (!kphp_error(third_node, "Cannot parse catch block"));
      auto try_vertex = VertexAdaptor<op_try>::create(embrace(first_node), second_node, embrace(third_node));
      set_location(try_vertex, try_location);
      return try_vertex;
    }
    case tok_inline_html: {
      auto html_code = VertexAdaptor<op_string>::create();
      set_location(html_code, AutoLocation(this));
      html_code->str_val = static_cast<string>((*cur)->str_val);

      auto echo_cmd = VertexAdaptor<op_echo>::create(html_code);
      set_location(echo_cmd, AutoLocation(this));
      next_cur();
      return echo_cmd;
    }
    case tok_at: {
      AutoLocation noerr_location(this);
      next_cur();
      first_node = get_statement();
      CE (first_node);
      auto noerr = VertexAdaptor<op_noerr>::create(first_node);
      set_location(noerr, noerr_location);
      return noerr;
    }
    case tok_clbrc: {
      return res;
    }
    case tok_const: {
      AutoLocation const_location(this);
      next_cur();

      bool has_access_modifier = std::distance(tokens->begin(), cur) > 1 && vk::any_of_equal((*std::prev(cur, 2))->type(), tok_public, tok_private, tok_protected);
      bool const_in_global_scope = in_func_cnt_ == 1 && !cur_class && processing_file->namespace_name.empty();
      bool const_in_class = in_func_cnt_ == 0 && cur_class;

      CE (!kphp_error(const_in_global_scope || const_in_class, "const expressions supported only inside classes and namespaces or in global scope"));
      CE (!kphp_error(test_expect(tok_func_name), "expected constant name"));
      CE (!kphp_error(!has_access_modifier, "unexpected const after private/protected/public keyword"));

      auto name = VertexAdaptor<op_string>::create();
      string const_name{(*cur)->str_val};

      if (const_in_class) {
        name->str_val = "c#" + replace_backslashes(cur_class->name) + "$$" + const_name;
      } else {
        name->str_val = const_name;
      }

      next_cur();
      CE (expect(tok_eq1, "'='"));
      VertexPtr v = get_expression();
      auto def = VertexAdaptor<op_define>::create(name, v);
      set_location(def, const_location);
      CE (check_statement_end());

      if (const_in_class) {
        cur_class->members.add_constant(def);
        auto empty = VertexAdaptor<op_empty>::create();
        return empty;
      }

      return def;
    }
    case tok_use: {
      AutoLocation const_location(this);
      CE (!kphp_error(!cur_class && in_func_cnt_ == 1, "'use' can be declared only in global scope"));
      get_use();
      auto empty = VertexAdaptor<op_empty>::create();
      return empty;
    }
    case tok_var: {
      next_cur();
      get_instance_var_list(phpdoc_token, access_public);
      CE (check_statement_end());
      auto empty = VertexAdaptor<op_empty>::create();
      return empty;
    }
    default:
      res = get_expression();
      if (!res) {
        if ((*cur)->type() == tok_semicolon) {
          auto empty = VertexAdaptor<op_empty>::create();
          set_location(empty, AutoLocation(this));
          res = empty;
        } else if (phpdoc_token) {
          return res;
        } else {
          CE (check_statement_end());
          return res;
        }
      } else {
        type_rule = get_type_rule();
        res->type_rule = type_rule;
        if (res->type() == op_set) {
          res.as<op_set>()->phpdoc_token = phpdoc_token;
        }
      }
      CE (check_statement_end());
      //CE (expect (tok_semicolon, "';'"));
      return res;
  }
  kphp_fail();
}

VertexPtr GenTree::get_instance_var_list(Token *phpdoc_token, AccessType access_type) {
  kphp_error(cur_class, "var declaration is outside of class");

  const vk::string_view &var_name = (*cur)->str_val;
  CE (expect(tok_var_name, "expected variable name"));

  VertexPtr def_val;
  if (test_expect(tok_eq1)) {
    next_cur();
    def_val = get_expression();
  }

  auto var = def_val ? VertexAdaptor<op_class_var>::create(def_val) : VertexAdaptor<op_class_var>::create();
  var->str_val = static_cast<string>(var_name);
  var->phpdoc_token = phpdoc_token;
  set_location(var, AutoLocation(this));

  cur_class->members.add_instance_field(var, access_type);

  if (test_expect(tok_comma)) {
    next_cur();
    get_instance_var_list(phpdoc_token, access_type);
  }

  return VertexPtr();
}

VertexPtr GenTree::get_seq() {
  vector<VertexPtr> seq_next;
  AutoLocation seq_location(this);

  while (cur != end && !test_expect(tok_clbrc)) {
    VertexPtr cur_node = get_statement();
    if (!cur_node) {
      continue;
    }
    seq_next.push_back(cur_node);
  }
  auto seq = VertexAdaptor<op_seq>::create(seq_next);
  set_location(seq, seq_location);

  return seq;
}

bool GenTree::has_return(VertexPtr v) {
  return v->type() == op_return || std::any_of(v->begin(), v->end(), has_return);
}

VertexPtr GenTree::run() {
  VertexPtr res;
  if (test_expect(tok_namespace)) {
    res = get_namespace_class();
  } else {
    res = get_statement();
  }
  kphp_assert (!res);
  if (cur != end) {
    fprintf(stderr, "line %d: something wrong\n", line_num);
    kphp_error (0, "Cannot compile (probably problems with brace balance)");
  }

  return res;
}

void GenTree::for_each(VertexPtr root, void (*callback)(VertexPtr)) {
  callback(root);

  for (auto i : *root) {
    for_each(i, callback);
  }
}

void php_gen_tree(vector<Token *> *tokens, SrcFilePtr file, DataStream<FunctionPtr> &os) {
  GenTree gen(tokens, file, os);
  gen.run();
}

#undef CE
