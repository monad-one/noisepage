#include "planner/plannodes/delete_plan_node.h"

#include <memory>
#include <utility>
#include <vector>

#include "common/json.h"
#include "planner/plannodes/output_schema.h"

namespace noisepage::planner {

// TODO(Gus,Wen) Add SetParameters

std::unique_ptr<DeletePlanNode> DeletePlanNode::Builder::Build() {
  return std::unique_ptr<DeletePlanNode>(
      new DeletePlanNode(std::move(children_), std::make_unique<OutputSchema>(), database_oid_, table_oid_));
}

DeletePlanNode::DeletePlanNode(std::vector<std::unique_ptr<AbstractPlanNode>> &&children,
                               std::unique_ptr<OutputSchema> output_schema, catalog::db_oid_t database_oid,
                               catalog::table_oid_t table_oid)
    : AbstractPlanNode(std::move(children), std::move(output_schema)),
      database_oid_(database_oid),
      table_oid_(table_oid) {}

common::hash_t DeletePlanNode::Hash() const {
  common::hash_t hash = AbstractPlanNode::Hash();

  // Hash database_oid
  hash = common::HashUtil::CombineHashes(hash, common::HashUtil::Hash(database_oid_));

  // Hash table_oid
  hash = common::HashUtil::CombineHashes(hash, common::HashUtil::Hash(table_oid_));

  return hash;
}

bool DeletePlanNode::operator==(const AbstractPlanNode &rhs) const {
  if (!AbstractPlanNode::operator==(rhs)) return false;

  auto &other = dynamic_cast<const DeletePlanNode &>(rhs);

  // Database OID
  if (database_oid_ != other.database_oid_) return false;

  // Table OID
  if (table_oid_ != other.table_oid_) return false;

  return true;
}

nlohmann::json DeletePlanNode::ToJson() const {
  nlohmann::json j = AbstractPlanNode::ToJson();
  j["database_oid"] = database_oid_;
  j["table_oid"] = table_oid_;
  return j;
}

std::vector<std::unique_ptr<parser::AbstractExpression>> DeletePlanNode::FromJson(const nlohmann::json &j) {
  std::vector<std::unique_ptr<parser::AbstractExpression>> exprs;
  auto e1 = AbstractPlanNode::FromJson(j);
  exprs.insert(exprs.end(), std::make_move_iterator(e1.begin()), std::make_move_iterator(e1.end()));
  database_oid_ = j.at("database_oid").get<catalog::db_oid_t>();
  table_oid_ = j.at("table_oid").get<catalog::table_oid_t>();
  return exprs;
}

DEFINE_JSON_BODY_DECLARATIONS(DeletePlanNode);

}  // namespace noisepage::planner
