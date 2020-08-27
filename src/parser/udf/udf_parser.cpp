#include <sstream>

#include "parser/udf/udf_parser.h"
#include "loggers/parser_logger.h"

#include "libpg_query/pg_query.h"
#include "nlohmann/json.hpp"

namespace terrier::parser::udf {
using namespace nlohmann;

const std::string kFunctionList = "FunctionList";
const std::string kDatums = "datums";
const std::string kPLpgSQL_var = "PLpgSQL_var";
const std::string kRefname = "refname";
const std::string kDatatype = "datatype";
const std::string kDefaultVal = "default_val";
const std::string kPLpgSQL_type = "PLpgSQL_type";
const std::string kTypname = "typname";
const std::string kAction = "action";
const std::string kPLpgSQL_function = "PLpgSQL_function";
const std::string kBody = "body";
const std::string kPLpgSQL_stmt_block = "PLpgSQL_stmt_block";
const std::string kPLpgSQL_stmt_return = "PLpgSQL_stmt_return";
const std::string kPLpgSQL_stmt_if = "PLpgSQL_stmt_if";
const std::string kPLpgSQL_stmt_while = "PLpgSQL_stmt_while";
const std::string kCond = "cond";
const std::string kThenBody = "then_body";
const std::string kElseBody = "else_body";
const std::string kExpr = "expr";
const std::string kQuery = "query";
const std::string kPLpgSQL_expr = "PLpgSQL_expr";
const std::string kPLpgSQL_stmt_assign = "PLpgSQL_stmt_assign";
const std::string kVarno = "varno";
const std::string kPLpgSQL_stmt_execsql = "PLpgSQL_stmt_execsql";
const std::string kSqlstmt = "sqlstmt";
const std::string kRow = "row";
const std::string kFields = "fields";
const std::string kName = "name";
const std::string kPLpgSQL_row = "PLpgSQL_row";
const std::string kPLpgSQL_stmt_dynexecute = "PLpgSQL_stmt_dynexecute";

std::unique_ptr<FunctionAST> PLpgSQLParser::ParsePLpgSQL(std::vector<std::string> &&param_names,
                                          std::vector<type::TypeId> &&param_types,
                                          const std::string &func_body,
                                          common::ManagedPointer<UDFASTContext> ast_context) {
  auto result = pg_query_parse_plpgsql("CREATE function add(x int) returns int language plpgsql as $$ begin return x + 1; end; $$;");
  if (result.error) {
    PARSER_LOG_INFO("PL/pgSQL parse error : {}", result.error->message);
    pg_query_free_plpgsql_parse_result(result);
    throw PARSER_EXCEPTION("PL/pgSQL parsing error");
  }
  // The result is a list, we need to wrap it
  std::string ast_json_str = "{ \"" + kFunctionList +
      "\" : " + std::string(result.plpgsql_funcs) + " }";
//  LOG_DEBUG("Compiling JSON formatted function %s", ast_json_str.c_str());
  pg_query_free_plpgsql_parse_result(result);

  std::cout << ast_json_str << "\n";

  std::istringstream ss(ast_json_str);
  json ast_json;
  ss >> ast_json;
  const auto function_list = ast_json[kFunctionList];
  TERRIER_ASSERT(function_list.is_array(), "Function list is not an array");
  if (function_list.size() != 1) {
    PARSER_LOG_DEBUG("PL/pgSQL error : Function list size %u", function_list.size());
    throw PARSER_EXCEPTION("Function list has size other than 1");
  }

  const auto function = function_list[0][kPLpgSQL_function];
  std::unique_ptr<FunctionAST> function_ast(
      new FunctionAST(ParseFunction(function), std::move(param_names), std::move(param_types)));
  return function_ast;
}

std::unique_ptr<StmtAST> PLpgSQLParser::ParseFunction(const nlohmann::json &block) {
  const auto decl_list = block[kDatums];
  const auto function_body = block[kAction][kPLpgSQL_stmt_block][kBody];

  std::vector<std::unique_ptr<StmtAST>> stmts;

  PARSER_LOG_DEBUG("Parsing Declarations");
  TERRIER_ASSERT(decl_list.is_array(), "Declaration list is not an array");
  for (uint32_t i = 1; i < decl_list.size(); i++) {
    stmts.push_back(ParseDecl(decl_list[i]));
  }

  stmts.push_back(ParseBlock(function_body));

  std::unique_ptr<SeqStmtAST> seq_stmt_ast(new SeqStmtAST(std::move(stmts)));
  return std::move(seq_stmt_ast);
}

std::unique_ptr<StmtAST> PLpgSQLParser::ParseBlock(const nlohmann::json &block) {
  // TODO(boweic): Support statements size other than 1
  TERRIER_ASSERT(block.is_array(), "Block isn't array");
  if (block.size() == 0) {
    throw PARSER_EXCEPTION("PL/pgSQL parser : Empty block is not supported");
  }

  std::vector<std::unique_ptr<StmtAST>> stmts;

  for (uint32_t i = 0; i < block.size(); i++) {
    const auto stmt = block[i];
    const auto stmt_names = stmt.items().begin();
//    TERRIER_ASSERT(stmt_names->size() == 1, "Bad statement size");
    PARSER_LOG_DEBUG("Statement : {}", stmt_names.key());

    if (stmt_names.key() == kPLpgSQL_stmt_return) {
      auto expr = ParseExprSQL(
          stmt[kPLpgSQL_stmt_return][kExpr][kPLpgSQL_expr][kQuery].get<std::string>());
      // TODO(boweic): Handle return stmt w/o expression
      std::unique_ptr<RetStmtAST> ret_stmt_ast(new RetStmtAST(std::move(expr)));
      stmts.push_back(std::move(ret_stmt_ast));
    } else if (stmt_names.key() == kPLpgSQL_stmt_if) {
      stmts.push_back(ParseIf(stmt[kPLpgSQL_stmt_if]));
    } else if (stmt_names.key() == kPLpgSQL_stmt_assign) {
      // TODO[Siva]: Need to fix Assignment expression / statement
      const std::string &var_name = udf_ast_context_->GetVariableAtIndex(stmt[kPLpgSQL_stmt_assign][kVarno].get<uint32_t >());
      std::unique_ptr<VariableExprAST> lhs(
          new VariableExprAST(var_name));
      auto rhs = ParseExprSQL(
          stmt[kPLpgSQL_stmt_assign][kExpr][kPLpgSQL_expr][kQuery].get<std::string>());
      std::unique_ptr<AssignStmtAST> ass_expr_ast(
          new AssignStmtAST(std::move(lhs), std::move(rhs)));
      stmts.push_back(std::move(ass_expr_ast));
    } else if (stmt_names.key() == kPLpgSQL_stmt_while) {
      stmts.push_back(ParseWhile(stmt[kPLpgSQL_stmt_while]));
    } else if (stmt_names.key() == kPLpgSQL_stmt_execsql) {
      stmts.push_back(ParseSQL(stmt[kPLpgSQL_stmt_execsql]));
    } else if (stmt_names.key() == kPLpgSQL_stmt_dynexecute) {
      stmts.push_back(ParseDynamicSQL(stmt[kPLpgSQL_stmt_dynexecute]));
    } else {
      throw PARSER_EXCEPTION("Statement type not supported");
    }
  }

  std::unique_ptr<SeqStmtAST> seq_stmt_ast(new SeqStmtAST(std::move(stmts)));
  return std::move(seq_stmt_ast);
}

std::unique_ptr<StmtAST> PLpgSQLParser::ParseDecl(const nlohmann::json &decl) {
  const auto &decl_names = decl.items().begin();
  for (auto& it : decl.items()) {
    std::cout << it.key() << " : " << it.value() << "\n";
  }
//  TERRIER_ASSERT(decl_names->size() >= 1, "Bad declaration names membership size");
  PARSER_LOG_DEBUG("Declaration : {}", decl_names.key());

  if (decl_names.key() == kPLpgSQL_var) {
    auto var_name = decl[kPLpgSQL_var][kRefname].get<std::string>();
    udf_ast_context_->AddVariable(var_name);
    auto type =
        decl[kPLpgSQL_var][kDatatype][kPLpgSQL_type][kTypname].get<std::string>();
    std::unique_ptr<ExprAST> initial = nullptr;
    if(decl[kPLpgSQL_var].find(kDefaultVal) != decl[kPLpgSQL_var].end()) {
      initial = ParseExprSQL(decl[kPLpgSQL_var][kDefaultVal][kPLpgSQL_expr][kQuery].get<std::string>());
    }


    PARSER_LOG_INFO("Registering type {0}: {1}", var_name.c_str(), type.c_str());

    type::TypeId temp_type;
    if(udf_ast_context_->GetVariableType(var_name, &temp_type)){
      return std::unique_ptr<DeclStmtAST>(new DeclStmtAST(var_name, temp_type, std::move(initial)));
    }

    if ((type.find("integer") != std::string::npos) || type.find("INTEGER") != std::string::npos) {
      udf_ast_context_->SetVariableType(var_name, type::TypeId::INTEGER);
      return std::unique_ptr<DeclStmtAST>(
          new DeclStmtAST(var_name, type::TypeId::INTEGER, std::move(initial)));
    } else if (type == "double") {
      udf_ast_context_->SetVariableType(var_name, type::TypeId::DECIMAL);
      return std::unique_ptr<DeclStmtAST>(
          new DeclStmtAST(var_name, type::TypeId::DECIMAL, std::move(initial)));
    } else if (type == "varchar") {
      udf_ast_context_->SetVariableType(var_name, type::TypeId::VARCHAR);
      return std::unique_ptr<DeclStmtAST>(
          new DeclStmtAST(var_name, type::TypeId::VARCHAR, std::move(initial)));
    } else {
      TERRIER_ASSERT(false, "Unsupported ");
//      udf_ast_context_->SetVariableType(var_name, type::TypeId::INVALID);
//      return std::unique_ptr<DeclStmtAST>(
//          new DeclStmtAST(var_name, type::TypeId::INVALID));
    }
  } else if (decl_names.key() == kPLpgSQL_row) {
    auto var_name = decl[kPLpgSQL_row][kRefname].get<std::string>();
    TERRIER_ASSERT(var_name == "*internal*", "Unexpected refname");
    // TODO[Siva]: Support row types later
    udf_ast_context_->SetVariableType(var_name, type::TypeId::INVALID);
    return std::unique_ptr<DeclStmtAST>(
        new DeclStmtAST(var_name, type::TypeId::INVALID, nullptr));

  }
  // TODO[Siva]: need to handle other types like row, table etc;
  throw PARSER_EXCEPTION("Declaration type not supported");
}

std::unique_ptr<StmtAST> PLpgSQLParser::ParseIf(const nlohmann::json &branch) {
  PARSER_LOG_DEBUG("ParseIf");
  auto cond_expr =
      ParseExprSQL(branch[kCond][kPLpgSQL_expr][kQuery].get<std::string>());
  auto then_stmt = ParseBlock(branch[kThenBody]);
  std::unique_ptr<StmtAST> else_stmt = nullptr;
  if(branch.find(kElseBody) != branch.end()){
    else_stmt = ParseBlock(branch[kElseBody]);
  }
  return std::unique_ptr<IfStmtAST>(new IfStmtAST(
      std::move(cond_expr), std::move(then_stmt), std::move(else_stmt)));
}

std::unique_ptr<StmtAST> PLpgSQLParser::ParseWhile(const nlohmann::json &loop) {
  PARSER_LOG_DEBUG("ParseWhile");
  auto cond_expr = ParseExprSQL(loop[kCond][kPLpgSQL_expr][kQuery].get<std::string>());
  auto body_stmt = ParseBlock(loop[kBody]);
  return std::unique_ptr<WhileStmtAST>(
      new WhileStmtAST(std::move(cond_expr), std::move(body_stmt)));
}

std::unique_ptr<StmtAST> PLpgSQLParser::ParseSQL(const nlohmann::json &sql_stmt) {
  PARSER_LOG_DEBUG("ParseSQL");
//  auto sql_query = sql_stmt[kSqlstmt][kPLpgSQL_expr][kQuery].get<std::string>();
//  auto var_name = sql_stmt[kRow][kPLpgSQL_row][kFields][0][kName].get<std::string>();
//  auto parse_result = PostgresParser::BuildParseTree(sql_query.c_str());
//  if (parse_result == nullptr) {
//    PARSER_LOG_DEBUG("Bad SQL statement");
//    return nullptr;
//  }
//  try {
//    // TODO(Matt): I don't think the binder should need the database name. It's already bound in the ConnectionContext
//    binder::BindNodeVisitor visitor(accessor_, db_name_);
//    visitor.BindNameToNode(parse_result->GetStatement(0), parse_result.get());
//  } catch (...) {
//    PARSER_LOG_DEBUG("Bad SQL statement");
//    return nullptr;
//  }



//  return std::unique_ptr<SQLStmtAST>(
//      new SQLStmtAST(std::move(sql_query), std::move(var_name)));
  return nullptr;
}

std::unique_ptr<StmtAST> PLpgSQLParser::ParseDynamicSQL(
    const nlohmann::json &sql_stmt) {
  PARSER_LOG_DEBUG("ParseDynamicSQL");
  auto sql_expr =
      ParseExprSQL(sql_stmt[kQuery][kPLpgSQL_expr][kQuery].get<std::string>());
  auto var_name = sql_stmt[kRow][kPLpgSQL_row][kFields][0][kName].get<std::string>();
  return std::unique_ptr<DynamicSQLStmtAST>(
      new DynamicSQLStmtAST(std::move(sql_expr), std::move(var_name)));
}

std::unique_ptr<ExprAST> PLpgSQLParser::ParseExprSQL(
    const std::string expr_sql_str) {
  PARSER_LOG_DEBUG("Parsing Expr SQL : {}", expr_sql_str.c_str());
  auto stmt_list = PostgresParser::BuildParseTree(expr_sql_str.c_str());
  if (stmt_list == nullptr) {
    return nullptr;
  }
  TERRIER_ASSERT(stmt_list->GetStatements().size() == 1, "Bad number of statements");
  auto stmt = stmt_list->GetStatement(0);
  TERRIER_ASSERT(stmt->GetType() == parser::StatementType::SELECT, "Unsupported statement type");
  TERRIER_ASSERT(stmt.CastManagedPointerTo<parser::SelectStatement>()->GetSelectTable() ==
      nullptr, "Unsupported SQL Expr in UDF");
  auto &select_list =
      stmt.CastManagedPointerTo<parser::SelectStatement>()->GetSelectColumns();
  TERRIER_ASSERT(select_list.size() == 1, "Unsupported number of select columns in udf");
  return PLpgSQLParser::ParseExpr(select_list[0]);
}

std::unique_ptr<ExprAST> PLpgSQLParser::ParseExpr(
    common::ManagedPointer<parser::AbstractExpression> expr) {
  if (expr->GetExpressionType() == parser::ExpressionType::COLUMN_VALUE) {
    return std::unique_ptr<VariableExprAST>(new VariableExprAST(
        expr.CastManagedPointerTo<parser::ColumnValueExpression>()
            ->GetColumnName()));
  } else if (parser::ExpressionUtil::IsOperatorExpression(
      expr->GetExpressionType()) ||
      (parser::ExpressionUtil::IsComparisonExpression(expr->GetExpressionType()))) {
    return std::unique_ptr<BinaryExprAST>(new BinaryExprAST(
        expr->GetExpressionType(), ParseExpr(expr->GetChild(0)),
        ParseExpr(expr->GetChild(1))));
  } else if (expr->GetExpressionType() == parser::ExpressionType::FUNCTION) {
    auto func_expr = expr.CastManagedPointerTo<parser::FunctionExpression>();
    std::vector<std::unique_ptr<ExprAST>> args;
    auto num_args = func_expr->GetChildrenSize();
    for (size_t idx = 0; idx < num_args; ++idx) {
      args.push_back(ParseExpr(func_expr->GetChild(idx)));
    }
    return std::unique_ptr<CallExprAST>(
        new CallExprAST(func_expr->GetFuncName(), std::move(args)));
  } else if (expr->GetExpressionType() == parser::ExpressionType::VALUE_CONSTANT) {
    return std::unique_ptr<ValueExprAST>(new ValueExprAST(
        expr->Copy()));
  }
  throw PARSER_EXCEPTION("PL/pgSQL parser : Expression type not supported");
}
}  // namespace terrier::parser::udf