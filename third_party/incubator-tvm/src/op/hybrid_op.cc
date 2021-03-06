/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \brief Hybrid computation rule.
 * \file hybrid_op.cc
 */

/*
 * 2019.12.30 - Add new implements for build provide.
 */

#include <tvm/operation.h>
#include <tvm/arithmetic.h>
#include <tvm/ir.h>
#include <tvm/ir_mutator.h>
#include <tvm/ir_pass.h>
#include <tvm/expr.h>
#include <tvm/expr_operator.h>
#include <unordered_set>
#include <string>
#include <utility>
#include <functional>
#include "op_util.h"
#include "hybrid_op.h"

namespace air {
using namespace ir;
// HybridOpNode
TVM_STATIC_IR_FUNCTOR(IRPrinter, vtable)
.set_dispatch<HybridOpNode>([](const ObjectRef& node, IRPrinter* p) {
    auto* op = static_cast<const HybridOpNode*>(node.get());
    p->stream << "hybrid(" << op->name << ", " << op << ")";
    p->stream << op->body << "\n";
  });

TVM_REGISTER_NODE_TYPE(HybridOpNode);

int HybridOpNode::num_outputs() const {
  return static_cast<int>(outputs.size());
}

Array<IterVar> HybridOpNode::root_iter_vars() const {
  return this->axis;
}

Type HybridOpNode::output_dtype(size_t i) const {
  return outputs[i]->dtype;
}

Array<Expr> HybridOpNode::output_shape(size_t i) const {
  return outputs[i]->shape;
}


Operation HybridOpNode::make(std::string name,
                             std::string tag,
                             Map<std::string, NodeRef> attrs,
                             Array<Tensor> inputs,
                             Array<Tensor> outputs,
                             Map<Tensor, Buffer> input_buffers,
                             Map<Tensor, Buffer> output_buffers, 
                             Map<Tensor, Region> input_regions,
                             Map<Tensor, Region> output_regions, 
                             Stmt body) {
  if (!attrs.defined()) {
    attrs = Map<std::string, NodeRef>();
  }
  auto n = make_node<HybridOpNode>();
  n->name = std::move(name);
  n->tag = std::move(tag);
  n->attrs = std::move(attrs);
  n->inputs = std::move(inputs);
  n->outputs = std::move(outputs);
  n->input_buffers_ = std::move(input_buffers);
  n->output_buffers_ = std::move(output_buffers);
  n->input_regions_ = std::move(input_regions);
  n->output_regions_ = std::move(output_regions);
  n->axis = op::GatherLoopVars(body);
  n->body = std::move(body);
  Operation res = Operation(n);
  return res;
}

Array<Tensor> HybridOpNode::InputTensors() const {
  return inputs;
}

Operation HybridOpNode::ReplaceInputs(
    const Operation &self,
    const std::unordered_map<Tensor, Tensor> &rmap) const {
  CHECK_EQ(self.operator->(), this);
  auto n = make_node<HybridOpNode>(*this);
  n->body = op::ReplaceTensor(this->body, rmap);
  for (size_t i = 0; i < n->inputs.size(); ++i) {
    Tensor t = n->inputs[i];
    if (rmap.count(t)) {
      n->inputs.Set(i, rmap.at(t));
    }
  }

  if (body.same_as(n->body) &&
      inputs.same_as(n->inputs)) {
    return self;
  } else {
    return Operation(n);
  }
}

void HybridOpNode::PropBoundToInputs(
    const Operation &self,
    arith::Analyzer* analyzer,
    const std::unordered_map<const Variable*, IntSet> &dom_map,
    std::unordered_map<Tensor, TensorDom>* out_dom_map) const {
  auto curr_inputs = InputTensors();
  for (Tensor t : curr_inputs) {
    auto it = out_dom_map->find(t);
    if (it == out_dom_map->end()) continue;
    TensorDom &dom = it->second;
    for (size_t i = 0; i < t->shape.size(); ++i) {
      dom.data[i].emplace_back(IntSet::range(
          Range::make_by_min_extent(
              make_const(t->shape[i].type(), 0), t->shape[i])));
    }
  }
}

void HybridOpNode::GatherBound(
    const Operation &self,
    const std::unordered_map<Tensor, TensorDom> &tensor_dom,
    std::unordered_map<IterVar, Range>* out_dom_map) const {
  for (auto iter_var : axis) {
    CHECK(!out_dom_map->count(iter_var));
    out_dom_map->operator[](iter_var) = iter_var->dom;
  }
}

Stmt HybridOpNode::BuildRealize(
    const Stage &stage,
    const std::unordered_map<IterVar, Range> &realize_map,
    const Stmt &body) const {
  // TODO(@were): Add attribute inject here and remove it from hybrid parser.
  CHECK_EQ(stage->op.get(), this);
  Stmt realize_body = body;
  for (int k = 0; k < num_outputs(); ++k) {
    Tensor t = stage->op.output(k);
    Region bounds;
    for (size_t i = 0; i < t->shape.size(); ++i) {
      bounds.push_back(
          Range::make_by_min_extent(
              make_const(t->shape[i].type(), 0), t->shape[i]));
    }
    realize_body = ir::Realize::make(
        t->op, t->value_index, t->dtype,
        bounds, const_true(), realize_body);
  }
  return realize_body;
}

Stmt HybridOpNode::BuildProvide(
    const Stage &stage,
    const std::unordered_map<IterVar, Range> &dom_map,
    bool debug_keep_trivial_loop) const {
  CHECK_EQ(stage->op.operator->(), this);
  Stmt ret = AttrStmt::make(make_zero(Int(32)), attr::extern_scope, 0, this->body);
  auto f_push_bind = [&ret](const Buffer& buffer, 
      const Tensor& tensor, const Region& region = Region()) {
    Array<NodeRef> bind_spec;
    Array<Expr> tuple;
    bind_spec.push_back(buffer);
    bind_spec.push_back(tensor);
    if (region.empty()) {
      for (size_t k = 0; k < buffer->shape.size(); ++k) {
        tuple.push_back(make_const(buffer->shape[k].type(), 0));
        tuple.push_back(buffer->shape[k]);
      }
    } else {
      for (size_t k = 0; k < region.size(); ++k) {
        tuple.push_back(region[k]->min);
        tuple.push_back(region[k]->extent);
      }
    }
    ret = AttrStmt::make(bind_spec, attr::buffer_bind_scope,
                         Call::make(Handle(), intrinsic::tvm_tuple, tuple, Call::Intrinsic), ret);
  };
  for (int i = static_cast<int>(outputs.size()) - 1; i >= 0; --i) {
    Buffer buffer;
    Region region;
    if (output_buffers_.count(outputs[i])) {
      buffer = output_buffers_[outputs[i]];
      region = output_regions_[outputs[i]];
    } else {
      buffer = decl_buffer(outputs[i]->shape, outputs[i]->dtype);
    }
    f_push_bind(buffer, stage->op.output(i), region);
  }
  for (int i = static_cast<int>(inputs.size()) - 1; i >= 0; --i) {
    Buffer buffer;
    Region region;
    if (input_buffers_.count(inputs[i])) {
      buffer = input_buffers_[inputs[i]];
      region = input_regions_[inputs[i]];
    } else {
      buffer = decl_buffer(inputs[i]->shape, inputs[i]->dtype);
    }
    f_push_bind(buffer, inputs[i], region);
  }

  std::unordered_map<Tensor, Tensor> rmap;
  for (int i = 0; i < this->num_outputs(); ++i) {
    rmap[outputs[i]] = stage->op.output(i);
  }
  auto n = make_node<HybridOpNode>(*this);
  /* This is a story little bit complicated.
   * The following two lines of codes replace output tensors' usage.
   * This is the simplest way I (@were) can come up with to glue
   * hybrid operation node to TVM op system.
   * In hybrid script all the tensors, especially the output tensors,
   * have their own names defined by the users. However, In TVM
   * conventional ops:
   *   1. Output tensors refer the corresponding op node so that the output
   *      tensors have the same names as the operation produces them.
   *   2. Once OpNode is wrapped up by an Operation node, it is finalized.
   *      Later access will be from a const OpNode*.
   * This is a chicken-egg paradox. It is impossible to put the output
   * tensors into the function body without forming the op node. The
   * function body is immutable after the node is formed.
   *
   * Finally, I decided to resolve this issue "lazily". During the
   * pipeline of compilation, this stage is a very preliminary stage.
   * Technically, it is before Phase 0. The actual tensors will be replaced
   * here.
   * Thus, the operation body is slightly different from the Phase 0 body.
   * This is a major difference that HybridOpNode is NOT the same as
   * ExternOpNode.
   * */
  ret = op::ReplaceTensor(ret, rmap);
  ret = op::ReplaceProvideTensor(ret, rmap);

  ret = op::ApplySchedule(stage, dom_map, ret);
  return ret;
}

namespace op {


Stmt ApplyLoopShapes(const Stage &stage,
                 const std::unordered_map<IterVar, Range> &dom_map, Stmt stmt) {
  class LoopSpliter : public IRMutator {
    Expr factor;
    const Variable *parent{nullptr};
    IterVar inner, outer;

   public:
    bool splitted{false};
    LoopSpliter(const SplitNode *split,
                const std::unordered_map<IterVar, Range> &dom_map) :
      factor(split->factor), splitted(false) {
      parent = split->parent->var.get();

      auto &inner_ = split->inner;
      CHECK(dom_map.count(inner_));
      auto &inner_dom = dom_map.find(inner_)->second;
      CHECK(is_const_int(inner_dom->min, 0));

      auto &outer_ = split->outer;
      CHECK(dom_map.count(outer_));
      auto &outer_dom = dom_map.find(outer_)->second;
      CHECK(is_const_int(outer_dom->min, 0));

      inner = IterVarNode::make(inner_dom, inner_->var, inner_->iter_type);
      outer = IterVarNode::make(outer_dom, outer_->var, outer_->iter_type);
    }
    ~LoopSpliter() override = default;

    Stmt Mutate_(const For *op, const Stmt &stmt) {
      if (op->loop_var.get() == parent) {
        std::unordered_map<const Variable *, Expr> rmap;
        rmap[op->loop_var.get()] = inner + outer * factor;
        Stmt ret = ir::Substitute(op->body, rmap);
        Expr cond = likely(outer * factor < (op->extent - inner));
        ret = IfThenElse::make(cond, ret);
        ret = For::make(inner->var, Expr(0), inner->dom->extent,
                        IterVarTypeToForType(inner->iter_type), op->device_api, ret);
        ret = For::make(outer->var, Expr(0), outer->dom->extent,
                        IterVarTypeToForType(outer->iter_type), op->device_api, ret);
        splitted = true;
        CHECK(ret.as<For>());
        return IRMutator::Mutate_(ret.as<For>(), ret);
      }
      return IRMutator::Mutate_(op, stmt);
    }

    Stmt Mutate_(const AttrStmt* op, const Stmt& stmt) override {
      if (op->attr_key != attr::buffer_bind_scope) {
        return IRMutator::Mutate_(op, stmt);
      }
      std::unordered_map<const Variable*, Expr> rmap;
      rmap[parent] = inner + outer * factor;
      Stmt ret = AttrStmt::make(op->node, op->attr_key, ir::Substitute(op->value, rmap), op->body);
      CHECK(ret.as<AttrStmt>());
      return IRMutator::Mutate_(ret.as<AttrStmt>(), ret);
    }
  };

  class LoopFuser : public IRMutator {
    const IterVar &parent;
    const Variable *inner{nullptr};
    const Variable *outer{nullptr};
    bool under_outer;
    Expr extent;

   public:
    bool fused{false};
    explicit LoopFuser(const FuseNode *fuse_)
      : parent(fuse_->fused), inner(fuse_->inner->var.get()),
        outer(fuse_->outer->var.get()), under_outer(false),
        extent(0), fused(false) {}
    ~LoopFuser() override = default;
    // TODO(@were): Handle imperfect loops

    Stmt Mutate_(const For *op, const Stmt &stmt) {
      if (op->loop_var.get() == inner) {
        CHECK(under_outer);
        std::unordered_map<const Variable *, Expr> rmap;
        rmap[op->loop_var.get()] = indexmod(parent, op->extent);
        extent = op->extent;
        fused = true;
        return ir::Substitute(op->body, rmap);
      } else if (op->loop_var.get() == outer) {
        under_outer = true;
        Stmt body = IRMutator::Mutate(op->body);
        std::unordered_map<const Variable *, Expr> rmap;
        rmap[op->loop_var.get()] = indexdiv(parent, extent);
        body = ir::Substitute(body, rmap);
        under_outer = false;
        return For::make(parent->var, Expr(0), extent * op->extent,
                         op->for_type, op->device_api, body);
      } else if (under_outer) {
        Stmt body = IRMutator::Mutate(op->body);
        std::unordered_map<const Variable *, Expr> rmap;
        rmap[op->loop_var.get()] = indexmod(indexdiv(parent, extent), op->extent);
        body = ir::Substitute(body, rmap);
        extent = extent * op->extent;
        return body;
      }
      return IRMutator::Mutate(stmt);
    }

    Stmt Mutate_(const AttrStmt* op, const Stmt& stmt) override {
      if (op->attr_key != attr::buffer_bind_scope) {
        return IRMutator::Mutate_(op, stmt);
      }
      Stmt body = IRMutator::Mutate(op->body);
      std::unordered_map<const Variable*, Expr> rmap;
      rmap[inner] = indexmod(parent, extent);
      rmap[outer] = indexdiv(parent, extent);
      return AttrStmt::make(op->node, op->attr_key, ir::Substitute(op->value, rmap), body);
    }
  };

  for (auto &rel : stage->relations) {
    if (const SplitNode *split = rel.as<SplitNode>()) {
      LoopSpliter Spliter(split, dom_map);
      stmt = Spliter.Mutate(stmt);
      CHECK(Spliter.splitted);
    } else if (const FuseNode *fuse = rel.as<FuseNode>()) {
      LoopFuser Fuser(fuse);
      stmt = Fuser.Mutate(stmt);
      CHECK(Fuser.fused);
    }
  }

  return stmt;
}

Stmt ApplyLoopAnnotations(const Stage &stage,
                          const std::unordered_map<IterVar, IterVar> &rebased, Stmt stmt) {
  class LoopAnnotator : public IRMutator {
    const Variable *var;
    const IterVarAttr &attr;

   public:
    LoopAnnotator(const Variable *var_, const IterVarAttr &attr_) : var(var_), attr(attr_) {}
    ~LoopAnnotator() override = default;

    Stmt Mutate_(const For *op, const Stmt &stmt) {
      if (op->loop_var.get() == var) {
        Stmt body = stmt;  // The return Stmt.
        if (attr->bind_thread.defined()) {
          const auto &iter_var = attr->bind_thread;
          if (iter_var->dom.defined()) {
            CHECK(is_const_int(iter_var->dom->min, 0));
            CHECK(Equal(iter_var->dom->extent, op->extent))
              << "Thread extent and loop extent mismatch!\n";
          }
          std::unordered_map<const Variable *, Expr> rmap;
          rmap[op->loop_var.get()] = iter_var;
          body = ir::Substitute(op->body, rmap);
          body = AttrStmt::make(iter_var, "thread_extent", op->extent, body);
        }
        // Handles the case of other for types.
        ForType expected = air::op::IterVarTypeToForType(attr->iter_type);
        if (!attr->bind_thread.defined() && expected != op->for_type) {
          body = For::make(op->loop_var, op->min, op->extent, IterVarTypeToForType(attr->iter_type),
                           op->device_api, op->body);
        }
        // Handles pragma annotations.
        CHECK_EQ(attr->pragma_keys.size(), attr->pragma_values.size());
        if (attr->pragma_keys.size()) {
          for (size_t k = 0; k < attr->pragma_keys.size(); ++k) {
            const std::string& pkey = attr->pragma_keys[k].as<StringImm>()->value;
            const Expr& pvalue = attr->pragma_values[k];
            body = AttrStmt::make(op->loop_var, ir::attr::pragma_scope_prefix + pkey, pvalue, body);
          }
        }
        return body;
      }
      return IRMutator::Mutate_(op, stmt);
    }
  };

  for (auto &iter_var : stage->leaf_iter_vars) {
    bool need_change = false;
    int found = 0;

    const IterVar &actual = rebased.count(iter_var) ? rebased.find(iter_var)->second : iter_var;
    const Variable *var = actual->var.get();
    ForType expected = IterVarTypeToForType(iter_var->iter_type);
    IterVarAttr attr;
    if (stage->iter_var_attrs.count(iter_var)) {
      attr = stage->iter_var_attrs[iter_var];
      expected = IterVarTypeToForType(attr->iter_type);
    }

    PostOrderVisit(stmt, [&found, &var, &attr, &expected, &need_change](const NodeRef &node) {
      if (const For *op = node.as<For>()) {
        if (op->loop_var.get() == var) {
          ++found;
          need_change = expected != op->for_type || (attr.defined() && attr->bind_thread.defined()) ||
                        (attr.defined() && attr->pragma_keys.size());
        }
      }
    });

    CHECK_EQ(found, 1) << " iter var should be found exactly once!";
    if (need_change) {
      stmt = LoopAnnotator(var, attr).Mutate(stmt);
    }
  }
  return stmt;
}

Stmt ApplyLoopOrder(const Stage &stage,
                    const std::unordered_map<IterVar, Range> &dom_map,
                    const std::unordered_map<IterVar, IterVar> &rebased, Stmt stmt) {
  //
  // Helpers that are needed for implementing reorder:
  //
  using VarOrder = std::vector<const Variable*>;

  // Get the current loop nest order (outermost to innermost).
  std::function<VarOrder(const Stmt&)> get_current_order = [](const Stmt& stmt) -> VarOrder {
    VarOrder current_order;
    PostOrderVisit(stmt, [&current_order](const NodeRef& node) {
      if (const For* op = node.as<For>()) current_order.push_back(op->loop_var.get());
    });
    std::reverse(current_order.begin(), current_order.end());
    return current_order;
  };

  // Get the required order (from rebase as much as possible; from outermost to
  // innermost).
  std::function<Array<IterVar>()> get_required_order = [&dom_map, &rebased, &stage]() -> Array<IterVar> {
    Array<IterVar> required_order;
    const Array<IterVar>& stage_order = stage->leaf_iter_vars;
    for (size_t i = 0; i < stage_order.size(); ++i) {
      const IterVar& iter_var = stage_order[i];
      const IterVar& required = rebased.count(iter_var) ? rebased.find(iter_var)->second : iter_var;
      CHECK(required->dom.defined() || dom_map.count(required)) << required << "\n";
      required_order.push_back(required);
    }
    return required_order;
  };
  const Array<IterVar> required_order = get_required_order();

  // Check if reorder is needed.
  std::function<bool(const VarOrder&)> is_reorder_needed = [&required_order](const VarOrder& current_order) -> bool {
    CHECK_EQ(current_order.size(), required_order.size()) << "Cannot reorder the loops!";
    for (size_t i = 0; i < current_order.size(); ++i) {
      if (current_order[i] != required_order[i]->var.get()) {
        return true;
      }
    }
    return false;
  };

  // Extracts For and AttrStmts that are related to the innermost iter var that
  // needs to be brought forward.
  class LoopInserter;  // Forward decl. for the friend class def. below.
  class LoopExtractor : public IRMutator {
    friend class LoopInserter;

   private:
    const VarOrder& current_order_;
    const Array<IterVar>& target_order_;
    const Variable* targeted_loop_iter_var_{nullptr};
    int j_;  // Where targeted_loop_iter_var_ is in target_order_.
    const For* targeted_loop_{nullptr};
    IterVar immediate_after_;
    std::vector<const AttrStmt*> associated_attr_stmts_;

   public:
    LoopExtractor(const VarOrder& current_order, const Array<IterVar>& target_order)
        : current_order_(current_order), target_order_(target_order), j_(0), associated_attr_stmts_() {
      // Search backwards to find the first loop that is behind the targeted
      // order.
      for (size_t i = current_order_.size(); i >= 1; --i) {
        if (current_order_[i - 1] == target_order_[i - 1]->var.get()) {
          continue;
        }
        for (size_t j = i - 1; j >= 1; --j) {
          if (current_order_[i - 1] == target_order_[j - 1]->var.get()) {
            targeted_loop_iter_var_ = current_order_[i - 1];
            immediate_after_ = target_order_[j];
            j_ = static_cast<int>(j - 1);
            return;
          }
        }
      }
      CHECK(false) << "There must be at least one loop out of order!";
    }
    ~LoopExtractor() override = default;

    Stmt Mutate_(const For* op, const Stmt& stmt) override {
      if (op->loop_var.get() != targeted_loop_iter_var_) {
        return IRMutator::Mutate_(op, stmt);
      }
      // This is the For node that we need to get rid of first.
      targeted_loop_ = op;
      return IRMutator::Mutate(op->body);
    }

    Stmt Mutate_(const AttrStmt* op, const Stmt& stmt) override {
      const auto attr_about = op->node.as<Variable>();
      if (attr_about == nullptr || attr_about != targeted_loop_iter_var_) {
        return IRMutator::Mutate_(op, stmt);
      }
      // This is the AttrStmt we are looking for.
      associated_attr_stmts_.push_back(op);
      return IRMutator::Mutate(op->body);
    }
  };

  // Insert the extracted For and AttrStmts into the correct places. Used
  // together with LoopExtractor.
  class LoopInserter : public IRMutator {
   private:
    const LoopExtractor& loop_extractor_;
    const Stage& stage_;
    const std::unordered_map<IterVar, Range>& dom_map_;

   public:
    LoopInserter(const LoopExtractor& loop_extractor, const Stage& stage,
                 const std::unordered_map<IterVar, Range>& dom_map)
        : loop_extractor_(loop_extractor), stage_(stage), dom_map_(dom_map) {}
    ~LoopInserter() override = default;

    Stmt Mutate_(const For* op, const Stmt& stmt) override {
      if (op->loop_var.get() != loop_extractor_.immediate_after_->var.get()) {
        return IRMutator::Mutate_(op, stmt);
      }
      // This is the loop we are looking for. Note, for the targeted loop, we
      // should create a new For using the (possibly rebased) IterVar in
      // required_order.
      const IterVar& target = loop_extractor_.target_order_[loop_extractor_.j_];
      ForType for_type = IterVarTypeToForType(target->iter_type);
      if (stage_->iter_var_attrs.count(target)) {
        for_type = IterVarTypeToForType(stage_->iter_var_attrs[target]->iter_type);
      }
      const Range& range = target->dom.defined() ? target->dom : dom_map_.find(target)->second;
      Stmt body = For::make(target->var, range->min, range->extent, for_type, DeviceAPI::None, stmt);
      // Go over the AttrStmt and attach them if there are any.
      for (auto it = loop_extractor_.associated_attr_stmts_.crbegin();
           it != loop_extractor_.associated_attr_stmts_.crend(); ++it) {
        body = AttrStmt::make((*it)->node, (*it)->attr_key, (*it)->value, body);
      }
      return body;
    }
  };

  //
  // The main algorithm:
  //   If any loop is behind its required position, move it to the position that
  //   is immediately before the loop that is supposed to be immediately after
  //   it in the required order. Repeat this process until the current order
  //   and the required order are the same.
  //
  // Example:
  //   current order: io ii jo ji
  //   required_order: ji ii io jo
  //   io ii jo ji -> io ji ii jo -> ii io ji jo -> ji ii io jo
  //
  // One can prove that all iter var accesses will be within scope. The
  // algorithm will terminate in O(n^2) in the worst case where n is the number
  // of loops.
  //
  VarOrder current_order = get_current_order(stmt);
  while (is_reorder_needed(current_order)) {
    LoopExtractor loop_extractor = LoopExtractor(current_order, required_order);
    LoopInserter loop_inserter = LoopInserter(loop_extractor, stage, dom_map);
    stmt = loop_inserter.Mutate(loop_extractor.Mutate(stmt));
    current_order = get_current_order(stmt);
  }

  return stmt;
}

Stmt ApplySchedule(const Stage &stage,
                   const std::unordered_map<IterVar, Range> &dom_map, Stmt stmt) {
  // TODO(@were): Eliminate loop rebase in script parser and move the burden here
  // Gather rebased variables
  std::unordered_map<IterVar, IterVar> rebased;
  for (auto rel : stage->relations) {
    if (const auto* rebase = rel.as<RebaseNode>()) {
      rebased[rebase->rebased] = rebase->parent;
      CHECK(rebase->parent->dom.defined());
      CHECK(dom_map.count(rebase->rebased));
    }
  }
  stmt = ApplyLoopShapes(stage, dom_map, stmt);
  stmt = ApplyLoopOrder(stage, dom_map, rebased, stmt);
  stmt = ApplyLoopAnnotations(stage, rebased, stmt);
  return stmt;
}

std::vector<IterVar> GatherLoopVars(Stmt stmt) {
  // TODO(@were): Write a comprehensive pass to analyze iter var types
  std::vector<IterVar> res_;
  PostOrderVisit(stmt, [&res_](const NodeRef &node) {
    if (const For *op = node.as<For>()) {
      Var loop_var(op->loop_var);
      Range dom = Range::make_by_min_extent(op->min, op->extent);
      res_.push_back(IterVarNode::make(dom, loop_var, ForTypeToIterVarType(op->for_type)));
    }
  });
  std::reverse(res_.begin(), res_.end());
  return res_;
}

// replacer to replace tensors' usage in Provide
class ProviderReplacer : public ir::IRMutator {
 public:
  explicit ProviderReplacer(const std::unordered_map<Tensor, Tensor> &vmap)
      : vmap_(vmap) {}
  ~ProviderReplacer() override = default;
  Stmt Mutate_(const ir::Provide* op, const Stmt &s) {
    Tensor t = Downcast<Operation>(op->func).output(op->value_index);
    auto it = vmap_.find(t);
    if (it != vmap_.end()) {
      Stmt ret = ir::Provide::make(
        it->second->op, it->second->value_index, op->value, op->args);
      found = true;
      CHECK(ret.as<ir::Provide>());
      return IRMutator::Mutate_(ret.as<ir::Provide>(), ret);
    }
    return IRMutator::Mutate_(op, s);
  }

  // whether it is found.
  bool found{false};

 private:
  const std::unordered_map<Tensor, Tensor> &vmap_;
};

Stmt ReplaceProvideTensor(Stmt stmt,
                   const std::unordered_map<Tensor, Tensor> &replace) {
  ProviderReplacer repl(replace);
  Stmt ret = repl.Mutate(stmt);
  return repl.found ? ret : stmt;
}
}  // namespace op
}  // namespace air
