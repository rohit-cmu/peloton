//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// binder_test.cpp
//
// Identification: test/binder/binder_test.cpp
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <memory>

#include "binder/bind_node_visitor.h"
#include "catalog/catalog.h"
#include "common/harness.h"
#include "common/statement.h"
#include "concurrency/transaction_manager_factory.h"
#include "expression/tuple_value_expression.h"
#include "expression/function_expression.h"
#include "optimizer/optimizer.h"
#include "parser/postgresparser.h"
#include "include/traffic_cop/traffic_cop.h"

#include <sql/testing_sql_util.h>
#include "type/value_factory.h"
#include "executor/testing_executor_util.h"

using std::string;
using std::unique_ptr;
using std::vector;
using std::make_tuple;

namespace peloton {
namespace test {

class BinderCorrectnessTest : public PelotonTest {
  virtual void SetUp() override {
    PelotonTest::SetUp();
    auto catalog = catalog::Catalog::GetInstance();
    catalog->Bootstrap();
    TestingExecutorUtil::InitializeDatabase(DEFAULT_DB_NAME);
  }

  virtual void TearDown() override {
    TestingExecutorUtil::DeleteDatabase(DEFAULT_DB_NAME);
    PelotonTest::TearDown();
  }
};

void SetupTables(std::string database_name) {
  LOG_INFO("Creating database %s", database_name.c_str());
  auto& txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  auto txn = txn_manager.BeginTransaction();
  catalog::Catalog::GetInstance()->CreateDatabase(database_name, txn);
  txn_manager.CommitTransaction(txn);
  LOG_INFO("database %s created!", database_name.c_str());

  auto& parser = parser::PostgresParser::GetInstance();
  auto& traffic_cop = tcop::TrafficCop::GetInstance();
  traffic_cop.SetDefaultDatabaseName(database_name);
  traffic_cop.SetTaskCallback(TestingSQLUtil::UtilTestTaskCallback,
                              &TestingSQLUtil::counter_);

  optimizer::Optimizer optimizer;

  vector<string> createTableSQLs{"CREATE TABLE A(A1 int, a2 varchar)",
                                 "CREATE TABLE b(B1 int, b2 varchar)"};
  for (auto& sql : createTableSQLs) {
    LOG_INFO("%s", sql.c_str());
    txn = txn_manager.BeginTransaction();
    traffic_cop.SetTcopTxnState(txn);

    vector<type::Value> params;
    vector<ResultValue> result;
    vector<int> result_format;
    unique_ptr<Statement> statement(new Statement("CREATE", sql));
    auto parse_tree = parser.BuildParseTree(sql);
    statement->SetPlanTree(
        optimizer.BuildPelotonPlanTree(parse_tree, database_name, txn));
    TestingSQLUtil::counter_.store(1);
    auto status = traffic_cop.ExecuteHelper(
        statement->GetPlanTree(), params, result, result_format);
    if (traffic_cop.GetQueuing()) {
      TestingSQLUtil::ContinueAfterComplete();
      traffic_cop.ExecuteStatementPlanGetResult();
      status = traffic_cop.p_status_;
      traffic_cop.SetQueuing(false);
    }
    LOG_INFO("Table create result: %s",
             ResultTypeToString(status.m_result).c_str());
    traffic_cop.CommitQueryHelper();
  }
}

TEST_F(BinderCorrectnessTest, SelectStatementTest) {
  std::string default_database_name = "TEST_DB";
  SetupTables(default_database_name);
  auto& parser = parser::PostgresParser::GetInstance();
  catalog::Catalog* catalog_ptr = catalog::Catalog::GetInstance();

  // Test regular table name
  LOG_INFO("Parsing sql query");

  auto& txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  auto txn = txn_manager.BeginTransaction();
  unique_ptr<binder::BindNodeVisitor> binder(
      new binder::BindNodeVisitor(txn, default_database_name));
  string selectSQL =
      "SELECT A.a1, B.b2 FROM A INNER JOIN b ON a.a1 = b.b1 "
      "WHERE a1 < 100 GROUP BY A.a1, B.b2 HAVING a1 > 50 "
      "ORDER BY a1";

  auto parse_tree = parser.BuildParseTree(selectSQL);
  auto selectStmt = dynamic_cast<parser::SelectStatement*>(
      parse_tree->GetStatements().at(0).get());
  binder->BindNameToNode(selectStmt);

  oid_t db_oid =
      catalog_ptr->GetDatabaseWithName(default_database_name, txn)->GetOid();
  oid_t tableA_oid =
      catalog_ptr->GetTableWithName(default_database_name, "a", txn)->GetOid();
  oid_t tableB_oid =
      catalog_ptr->GetTableWithName(default_database_name, "b", txn)->GetOid();
  txn_manager.CommitTransaction(txn);

  // Check select_list
  LOG_INFO("Checking select list");
  auto tupleExpr = dynamic_cast<const expression::TupleValueExpression*>(
      selectStmt->select_list[0].get());
  EXPECT_EQ(tupleExpr->GetBoundOid(),
            make_tuple(db_oid, tableA_oid, 0));  // A.a1
  EXPECT_EQ(type::TypeId::INTEGER, tupleExpr->GetValueType());
  tupleExpr =
      (expression::TupleValueExpression*)selectStmt->select_list[1].get();
  EXPECT_EQ(tupleExpr->GetBoundOid(),
            make_tuple(db_oid, tableB_oid, 1));  // B.b2
  EXPECT_EQ(type::TypeId::VARCHAR, tupleExpr->GetValueType());

  // Check join condition
  LOG_INFO("Checking join condition");
  tupleExpr = (expression::TupleValueExpression*)
                  selectStmt->from_table->join->condition->GetChild(0);
  EXPECT_EQ(tupleExpr->GetBoundOid(),
            make_tuple(db_oid, tableA_oid, 0));  // a.a1
  tupleExpr = (expression::TupleValueExpression*)
                  selectStmt->from_table->join->condition->GetChild(1);
  EXPECT_EQ(tupleExpr->GetBoundOid(),
            make_tuple(db_oid, tableB_oid, 0));  // b.b1

  // Check Where clause
  LOG_INFO("Checking where clause");
  tupleExpr =
      (expression::TupleValueExpression*)selectStmt->where_clause->GetChild(0);
  EXPECT_EQ(tupleExpr->GetBoundOid(), make_tuple(db_oid, tableA_oid, 0));  // a1

  // Check Group By and Having
  LOG_INFO("Checking group by");
  tupleExpr =
      (expression::TupleValueExpression*)selectStmt->group_by->columns.at(0)
          .get();
  EXPECT_EQ(tupleExpr->GetBoundOid(),
            make_tuple(db_oid, tableA_oid, 0));  // A.a1
  tupleExpr = dynamic_cast<const expression::TupleValueExpression*>(
      selectStmt->group_by->columns.at(1).get());
  EXPECT_EQ(tupleExpr->GetBoundOid(),
            make_tuple(db_oid, tableB_oid, 1));  // B.b2
  tupleExpr =
      (expression::TupleValueExpression*)selectStmt->group_by->having->GetChild(
          0);
  EXPECT_EQ(tupleExpr->GetBoundOid(), make_tuple(db_oid, tableA_oid, 0));  // a1

  // Check Order By
  LOG_INFO("Checking order by");
  tupleExpr =
      (expression::TupleValueExpression*)selectStmt->order->exprs.at(0).get();
  EXPECT_EQ(tupleExpr->GetBoundOid(), make_tuple(db_oid, tableA_oid, 0));  // a1

  // Check alias ambiguous
  LOG_INFO("Checking duplicate alias and table name.");

  txn = txn_manager.BeginTransaction();
  binder.reset(new binder::BindNodeVisitor(txn, default_database_name));
  selectSQL = "SELECT * FROM A, B as A";
  parse_tree = parser.BuildParseTree(selectSQL);
  selectStmt = dynamic_cast<parser::SelectStatement*>(
      parse_tree->GetStatements().at(0).get());
  try {
    binder->BindNameToNode(selectStmt);
    EXPECT_TRUE(false);
  } catch (Exception& e) {
    LOG_INFO("Correct! Exception(%s) catched", e.what());
  }

  // Test select from different table instances from the same physical schema
  txn_manager.CommitTransaction(txn);

  txn = txn_manager.BeginTransaction();
  binder.reset(new binder::BindNodeVisitor(txn, default_database_name));
  selectSQL = "SELECT * FROM A, A as AA where A.a1 = AA.a2";
  parse_tree = parser.BuildParseTree(selectSQL);
  selectStmt = dynamic_cast<parser::SelectStatement*>(
      parse_tree->GetStatements().at(0).get());
  binder->BindNameToNode(selectStmt);
  LOG_INFO("Checking where clause");
  tupleExpr = dynamic_cast<const expression::TupleValueExpression*>(
      selectStmt->where_clause->GetChild(0));
  EXPECT_EQ(tupleExpr->GetBoundOid(), make_tuple(db_oid, tableA_oid, 0));  // a1
  tupleExpr = dynamic_cast<const expression::TupleValueExpression*>(
      selectStmt->where_clause->GetChild(1));
  EXPECT_EQ(tupleExpr->GetBoundOid(), make_tuple(db_oid, tableA_oid, 1));  // a1

  // Test alias and select_list
  LOG_INFO("Checking select_list and table alias binding");
  txn_manager.CommitTransaction(txn);

  txn = txn_manager.BeginTransaction();
  binder.reset(new binder::BindNodeVisitor(txn, default_database_name));
  selectSQL = "SELECT AA.a1, b2 FROM A as AA, B WHERE AA.a1 = B.b1";
  parse_tree = parser.BuildParseTree(selectSQL);
  selectStmt = dynamic_cast<parser::SelectStatement*>(
      parse_tree->GetStatements().at(0).get());
  binder->BindNameToNode(selectStmt);
  tupleExpr = dynamic_cast<expression::TupleValueExpression*>(
      selectStmt->select_list.at(0).get());
  EXPECT_EQ(tupleExpr->GetBoundOid(), make_tuple(db_oid, tableA_oid, 0));
  tupleExpr = dynamic_cast<expression::TupleValueExpression*>(
      selectStmt->select_list.at(1).get());
  EXPECT_EQ(tupleExpr->GetBoundOid(), make_tuple(db_oid, tableB_oid, 1));
  txn_manager.CommitTransaction(txn);
  // Delete the test database
  txn = txn_manager.BeginTransaction();
  catalog_ptr->DropDatabaseWithName(default_database_name, txn);
  txn_manager.CommitTransaction(txn);
}

// TODO: add test for Update Statement. Currently UpdateStatement uses char*
// instead of TupleValueExpression to represent column. We can only add this
// test after UpdateStatement is changed

TEST_F(BinderCorrectnessTest, DeleteStatementTest) {
  std::string default_database_name = "TEST_DB";
  SetupTables(default_database_name);
  auto& parser = parser::PostgresParser::GetInstance();
  catalog::Catalog* catalog_ptr = catalog::Catalog::GetInstance();

  auto& txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  auto txn = txn_manager.BeginTransaction();
  oid_t db_oid =
      catalog_ptr->GetDatabaseWithName(default_database_name, txn)->GetOid();
  oid_t tableB_oid =
      catalog_ptr->GetTableWithName(default_database_name, "b", txn)->GetOid();

  string deleteSQL = "DELETE FROM b WHERE 1 = b1 AND b2 = 'str'";
  unique_ptr<binder::BindNodeVisitor> binder(
      new binder::BindNodeVisitor(txn, default_database_name));

  auto parse_tree = parser.BuildParseTree(deleteSQL);
  auto deleteStmt = dynamic_cast<parser::DeleteStatement*>(
      parse_tree->GetStatements().at(0).get());
  binder->BindNameToNode(deleteStmt);

  txn_manager.CommitTransaction(txn);

  LOG_INFO("Checking first condition in where clause");
  auto tupleExpr = dynamic_cast<const expression::TupleValueExpression*>(
      deleteStmt->expr->GetChild(0)->GetChild(1));
  EXPECT_EQ(tupleExpr->GetBoundOid(), make_tuple(db_oid, tableB_oid, 0));

  LOG_INFO("Checking second condition in where clause");
  tupleExpr = dynamic_cast<const expression::TupleValueExpression*>(
      deleteStmt->expr->GetChild(1)->GetChild(0));
  EXPECT_EQ(tupleExpr->GetBoundOid(), make_tuple(db_oid, tableB_oid, 1));

  // Delete the test database
  txn = txn_manager.BeginTransaction();
  catalog_ptr->DropDatabaseWithName(default_database_name, txn);
  txn_manager.CommitTransaction(txn);
}

TEST_F(BinderCorrectnessTest, FunctionExpressionTest) {
  auto& txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  auto txn = txn_manager.BeginTransaction();

  string function_sql = "SELECT substr('test123', a, 3)";
  auto& parser = parser::PostgresParser::GetInstance();
  auto parse_tree = parser.BuildParseTree(function_sql);
  auto stmt = parse_tree->GetStatement(0);
  unique_ptr<binder::BindNodeVisitor> binder(
      new binder::BindNodeVisitor(txn, DEFAULT_DB_NAME));
  EXPECT_THROW(binder->BindNameToNode(stmt), peloton::BinderException);

  function_sql = "SELECT substr('test123', 2, 3)";
  auto parse_tree2 = parser.BuildParseTree(function_sql);
  stmt = parse_tree2->GetStatement(0);
  binder->BindNameToNode(stmt);
  auto funct_expr = dynamic_cast<expression::FunctionExpression*>(
      dynamic_cast<parser::SelectStatement*>(stmt)->select_list[0].get());
  EXPECT_TRUE(funct_expr->Evaluate(nullptr, nullptr, nullptr)
                  .CompareEquals(type::ValueFactory::GetVarcharValue("est")) ==
              type::CmpBool::TRUE);

  txn_manager.CommitTransaction(txn);
}

}  // namespace test
}  // namespace peloton
